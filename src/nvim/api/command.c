// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "klib/kvec.h"
#include "lauxlib.h"
#include "nvim/api/command.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/helpers.h"
#include "nvim/ascii.h"
#include "nvim/autocmd.h"
#include "nvim/buffer_defs.h"
#include "nvim/decoration.h"
#include "nvim/ex_cmds.h"
#include "nvim/ex_docmd.h"
#include "nvim/ex_eval.h"
#include "nvim/garray.h"
#include "nvim/globals.h"
#include "nvim/lua/executor.h"
#include "nvim/macros.h"
#include "nvim/mbyte.h"
#include "nvim/memory.h"
#include "nvim/ops.h"
#include "nvim/pos.h"
#include "nvim/regexp.h"
#include "nvim/strings.h"
#include "nvim/types.h"
#include "nvim/usercmd.h"
#include "nvim/vim.h"
#include "nvim/window.h"

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "api/command.c.generated.h"
#endif

/// Parse command line.
///
/// Doesn't check the validity of command arguments.
///
/// @param str       Command line string to parse. Cannot contain "\n".
/// @param opts      Optional parameters. Reserved for future use.
/// @param[out] err  Error details, if any.
/// @return Dictionary containing command information, with these keys:
///         - cmd: (string) Command name.
///         - range: (array) (optional) Command range (|<line1>| |<line2>|).
///                          Omitted if command doesn't accept a range.
///                          Otherwise, has no elements if no range was specified, one element if
///                          only a single range item was specified, or two elements if both range
///                          items were specified.
///         - count: (number) (optional) Command |<count>|.
///                           Omitted if command cannot take a count.
///         - reg: (string) (optional) Command |<register>|.
///                         Omitted if command cannot take a register.
///         - bang: (boolean) Whether command contains a |<bang>| (!) modifier.
///         - args: (array) Command arguments.
///         - addr: (string) Value of |:command-addr|. Uses short name or "line" for -addr=lines.
///         - nargs: (string) Value of |:command-nargs|.
///         - nextcmd: (string) Next command if there are multiple commands separated by a |:bar|.
///                             Empty if there isn't a next command.
///         - magic: (dictionary) Which characters have special meaning in the command arguments.
///             - file: (boolean) The command expands filenames. Which means characters such as "%",
///                               "#" and wildcards are expanded.
///             - bar: (boolean) The "|" character is treated as a command separator and the double
///                              quote character (\") is treated as the start of a comment.
///         - mods: (dictionary) |:command-modifiers|.
///             - filter: (dictionary) |:filter|.
///                 - pattern: (string) Filter pattern. Empty string if there is no filter.
///                 - force: (boolean) Whether filter is inverted or not.
///             - silent: (boolean) |:silent|.
///             - emsg_silent: (boolean) |:silent!|.
///             - unsilent: (boolean) |:unsilent|.
///             - sandbox: (boolean) |:sandbox|.
///             - noautocmd: (boolean) |:noautocmd|.
///             - browse: (boolean) |:browse|.
///             - confirm: (boolean) |:confirm|.
///             - hide: (boolean) |:hide|.
///             - horizontal: (boolean) |:horizontal|.
///             - keepalt: (boolean) |:keepalt|.
///             - keepjumps: (boolean) |:keepjumps|.
///             - keepmarks: (boolean) |:keepmarks|.
///             - keeppatterns: (boolean) |:keeppatterns|.
///             - lockmarks: (boolean) |:lockmarks|.
///             - noswapfile: (boolean) |:noswapfile|.
///             - tab: (integer) |:tab|. -1 when omitted.
///             - verbose: (integer) |:verbose|. -1 when omitted.
///             - vertical: (boolean) |:vertical|.
///             - split: (string) Split modifier string, is an empty string when there's no split
///                               modifier. If there is a split modifier it can be one of:
///               - "aboveleft": |:aboveleft|.
///               - "belowright": |:belowright|.
///               - "topleft": |:topleft|.
///               - "botright": |:botright|.
Dictionary nvim_parse_cmd(String str, Dictionary opts, Error *err)
  FUNC_API_SINCE(10) FUNC_API_FAST
{
  Dictionary result = ARRAY_DICT_INIT;

  if (opts.size > 0) {
    api_set_error(err, kErrorTypeValidation, "opts dict isn't empty");
    return result;
  }

  // Parse command line
  exarg_T ea;
  CmdParseInfo cmdinfo;
  char *cmdline = string_to_cstr(str);
  char *errormsg = NULL;

  if (!parse_cmdline(cmdline, &ea, &cmdinfo, &errormsg)) {
    if (errormsg != NULL) {
      api_set_error(err, kErrorTypeException, "Error while parsing command line: %s", errormsg);
    } else {
      api_set_error(err, kErrorTypeException, "Error while parsing command line");
    }
    goto end;
  }

  // Parse arguments
  Array args = ARRAY_DICT_INIT;
  size_t length = strlen(ea.arg);

  // For nargs = 1 or '?', pass the entire argument list as a single argument,
  // otherwise split arguments by whitespace.
  if (ea.argt & EX_NOSPC) {
    if (*ea.arg != NUL) {
      ADD(args, STRING_OBJ(cstrn_to_string(ea.arg, length)));
    }
  } else {
    size_t end = 0;
    size_t len = 0;
    char *buf = xcalloc(length, sizeof(char));
    bool done = false;

    while (!done) {
      done = uc_split_args_iter(ea.arg, length, &end, buf, &len);
      if (len > 0) {
        ADD(args, STRING_OBJ(cstrn_to_string(buf, len)));
      }
    }

    xfree(buf);
  }

  ucmd_T *cmd = NULL;
  if (ea.cmdidx == CMD_USER) {
    cmd = USER_CMD(ea.useridx);
  } else if (ea.cmdidx == CMD_USER_BUF) {
    cmd = USER_CMD_GA(&curbuf->b_ucmds, ea.useridx);
  }

  if (cmd != NULL) {
    PUT(result, "cmd", CSTR_TO_OBJ(cmd->uc_name));
  } else {
    PUT(result, "cmd", CSTR_TO_OBJ(get_command_name(NULL, ea.cmdidx)));
  }

  if (ea.argt & EX_RANGE) {
    Array range = ARRAY_DICT_INIT;
    if (ea.addr_count > 0) {
      if (ea.addr_count > 1) {
        ADD(range, INTEGER_OBJ(ea.line1));
      }
      ADD(range, INTEGER_OBJ(ea.line2));
    }
    PUT(result, "range", ARRAY_OBJ(range));
  }

  if (ea.argt & EX_COUNT) {
    if (ea.addr_count > 0) {
      PUT(result, "count", INTEGER_OBJ(ea.line2));
    } else if (cmd != NULL) {
      PUT(result, "count", INTEGER_OBJ(cmd->uc_def));
    } else {
      PUT(result, "count", INTEGER_OBJ(0));
    }
  }

  if (ea.argt & EX_REGSTR) {
    char reg[2] = { (char)ea.regname, NUL };
    PUT(result, "reg", CSTR_TO_OBJ(reg));
  }

  PUT(result, "bang", BOOLEAN_OBJ(ea.forceit));
  PUT(result, "args", ARRAY_OBJ(args));

  char nargs[2];
  if (ea.argt & EX_EXTRA) {
    if (ea.argt & EX_NOSPC) {
      if (ea.argt & EX_NEEDARG) {
        nargs[0] = '1';
      } else {
        nargs[0] = '?';
      }
    } else if (ea.argt & EX_NEEDARG) {
      nargs[0] = '+';
    } else {
      nargs[0] = '*';
    }
  } else {
    nargs[0] = '0';
  }
  nargs[1] = '\0';
  PUT(result, "nargs", CSTR_TO_OBJ(nargs));

  const char *addr;
  switch (ea.addr_type) {
  case ADDR_LINES:
    addr = "line";
    break;
  case ADDR_ARGUMENTS:
    addr = "arg";
    break;
  case ADDR_BUFFERS:
    addr = "buf";
    break;
  case ADDR_LOADED_BUFFERS:
    addr = "load";
    break;
  case ADDR_WINDOWS:
    addr = "win";
    break;
  case ADDR_TABS:
    addr = "tab";
    break;
  case ADDR_QUICKFIX:
    addr = "qf";
    break;
  case ADDR_NONE:
    addr = "none";
    break;
  default:
    addr = "?";
    break;
  }
  PUT(result, "addr", CSTR_TO_OBJ(addr));
  PUT(result, "nextcmd", CSTR_TO_OBJ(ea.nextcmd));

  Dictionary mods = ARRAY_DICT_INIT;

  Dictionary filter = ARRAY_DICT_INIT;
  PUT(filter, "pattern", cmdinfo.cmdmod.cmod_filter_pat
      ? CSTR_TO_OBJ(cmdinfo.cmdmod.cmod_filter_pat)
      : STRING_OBJ(STATIC_CSTR_TO_STRING("")));
  PUT(filter, "force", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_filter_force));
  PUT(mods, "filter", DICTIONARY_OBJ(filter));

  PUT(mods, "silent", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_SILENT));
  PUT(mods, "emsg_silent", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_ERRSILENT));
  PUT(mods, "unsilent", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_UNSILENT));
  PUT(mods, "sandbox", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_SANDBOX));
  PUT(mods, "noautocmd", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_NOAUTOCMD));
  PUT(mods, "tab", INTEGER_OBJ(cmdinfo.cmdmod.cmod_tab - 1));
  PUT(mods, "verbose", INTEGER_OBJ(cmdinfo.cmdmod.cmod_verbose - 1));
  PUT(mods, "browse", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_BROWSE));
  PUT(mods, "confirm", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_CONFIRM));
  PUT(mods, "hide", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_HIDE));
  PUT(mods, "keepalt", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_KEEPALT));
  PUT(mods, "keepjumps", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_KEEPJUMPS));
  PUT(mods, "keepmarks", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_KEEPMARKS));
  PUT(mods, "keeppatterns", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_KEEPPATTERNS));
  PUT(mods, "lockmarks", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_LOCKMARKS));
  PUT(mods, "noswapfile", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_flags & CMOD_NOSWAPFILE));
  PUT(mods, "vertical", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_split & WSP_VERT));
  PUT(mods, "horizontal", BOOLEAN_OBJ(cmdinfo.cmdmod.cmod_split & WSP_HOR));

  const char *split;
  if (cmdinfo.cmdmod.cmod_split & WSP_BOT) {
    split = "botright";
  } else if (cmdinfo.cmdmod.cmod_split & WSP_TOP) {
    split = "topleft";
  } else if (cmdinfo.cmdmod.cmod_split & WSP_BELOW) {
    split = "belowright";
  } else if (cmdinfo.cmdmod.cmod_split & WSP_ABOVE) {
    split = "aboveleft";
  } else {
    split = "";
  }
  PUT(mods, "split", CSTR_TO_OBJ(split));

  PUT(result, "mods", DICTIONARY_OBJ(mods));

  Dictionary magic = ARRAY_DICT_INIT;
  PUT(magic, "file", BOOLEAN_OBJ(cmdinfo.magic.file));
  PUT(magic, "bar", BOOLEAN_OBJ(cmdinfo.magic.bar));
  PUT(result, "magic", DICTIONARY_OBJ(magic));

  undo_cmdmod(&cmdinfo.cmdmod);
end:
  xfree(cmdline);
  return result;
}

/// Executes an Ex command.
///
/// Unlike |nvim_command()| this command takes a structured Dictionary instead of a String. This
/// allows for easier construction and manipulation of an Ex command. This also allows for things
/// such as having spaces inside a command argument, expanding filenames in a command that otherwise
/// doesn't expand filenames, etc. Command arguments may also be Number, Boolean or String.
///
/// The first argument may also be used instead of count for commands that support it in order to
/// make their usage simpler with |vim.cmd()|. For example, instead of
/// `vim.cmd.bdelete{ count = 2 }`, you may do `vim.cmd.bdelete(2)`.
///
/// On execution error: fails with VimL error, updates v:errmsg.
///
/// @see |nvim_exec()|
/// @see |nvim_command()|
///
/// @param cmd       Command to execute. Must be a Dictionary that can contain the same values as
///                  the return value of |nvim_parse_cmd()| except "addr", "nargs" and "nextcmd"
///                  which are ignored if provided. All values except for "cmd" are optional.
/// @param opts      Optional parameters.
///                  - output: (boolean, default false) Whether to return command output.
/// @param[out] err  Error details, if any.
/// @return Command output (non-error, non-shell |:!|) if `output` is true, else empty string.
String nvim_cmd(uint64_t channel_id, Dict(cmd) *cmd, Dict(cmd_opts) *opts, Error *err)
  FUNC_API_SINCE(10)
{
  exarg_T ea;
  CLEAR_FIELD(ea);

  CmdParseInfo cmdinfo;
  CLEAR_FIELD(cmdinfo);

  char *cmdline = NULL;
  char *cmdname = NULL;
  ArrayOf(String) args = ARRAY_DICT_INIT;

  String retv = (String)STRING_INIT;

#define OBJ_TO_BOOL(var, value, default, varname) \
  do { \
    (var) = api_object_to_bool(value, varname, default, err); \
    if (ERROR_SET(err)) { \
      goto end; \
    } \
  } while (0)

#define OBJ_TO_CMOD_FLAG(flag, value, default, varname) \
  do { \
    if (api_object_to_bool(value, varname, default, err)) { \
      cmdinfo.cmdmod.cmod_flags |= (flag); \
    } \
    if (ERROR_SET(err)) { \
      goto end; \
    } \
  } while (0)

#define VALIDATION_ERROR(...) \
  do { \
    api_set_error(err, kErrorTypeValidation, __VA_ARGS__); \
    goto end; \
  } while (0)

  bool output;
  OBJ_TO_BOOL(output, opts->output, false, "'output'");

  // First, parse the command name and check if it exists and is valid.
  if (!HAS_KEY(cmd->cmd) || cmd->cmd.type != kObjectTypeString
      || cmd->cmd.data.string.data[0] == NUL) {
    VALIDATION_ERROR("'cmd' must be a non-empty String");
  }

  cmdname = string_to_cstr(cmd->cmd.data.string);
  ea.cmd = cmdname;

  char *p = find_ex_command(&ea, NULL);

  // If this looks like an undefined user command and there are CmdUndefined
  // autocommands defined, trigger the matching autocommands.
  if (p != NULL && ea.cmdidx == CMD_SIZE && ASCII_ISUPPER(*ea.cmd)
      && has_event(EVENT_CMDUNDEFINED)) {
    p = xstrdup(cmdname);
    int ret = apply_autocmds(EVENT_CMDUNDEFINED, p, p, true, NULL);
    xfree(p);
    // If the autocommands did something and didn't cause an error, try
    // finding the command again.
    p = (ret && !aborting()) ? find_ex_command(&ea, NULL) : ea.cmd;
  }

  if (p == NULL || ea.cmdidx == CMD_SIZE) {
    VALIDATION_ERROR("Command not found: %s", cmdname);
  }
  if (is_cmd_ni(ea.cmdidx)) {
    VALIDATION_ERROR("Command not implemented: %s", cmdname);
  }

  // Get the command flags so that we can know what type of arguments the command uses.
  // Not required for a user command since `find_ex_command` already deals with it in that case.
  if (!IS_USER_CMDIDX(ea.cmdidx)) {
    ea.argt = get_cmd_argt(ea.cmdidx);
  }

  // Parse command arguments since it's needed to get the command address type.
  if (HAS_KEY(cmd->args)) {
    if (cmd->args.type != kObjectTypeArray) {
      VALIDATION_ERROR("'args' must be an Array");
    }

    // Process all arguments. Convert non-String arguments to String and check if String arguments
    // have non-whitespace characters.
    for (size_t i = 0; i < cmd->args.data.array.size; i++) {
      Object elem = cmd->args.data.array.items[i];
      char *data_str;

      switch (elem.type) {
      case kObjectTypeBoolean:
        data_str = xcalloc(2, sizeof(char));
        data_str[0] = elem.data.boolean ? '1' : '0';
        data_str[1] = '\0';
        break;
      case kObjectTypeBuffer:
      case kObjectTypeWindow:
      case kObjectTypeTabpage:
      case kObjectTypeInteger:
        data_str = xcalloc(NUMBUFLEN, sizeof(char));
        snprintf(data_str, NUMBUFLEN, "%" PRId64, elem.data.integer);
        break;
      case kObjectTypeString:
        if (string_iswhite(elem.data.string)) {
          VALIDATION_ERROR("String command argument must have at least one non-whitespace "
                           "character");
        }
        data_str = xstrndup(elem.data.string.data, elem.data.string.size);
        break;
      default:
        VALIDATION_ERROR("Invalid type for command argument");
        break;
      }

      ADD(args, STRING_OBJ(cstr_as_string(data_str)));
    }

    bool argc_valid;

    // Check if correct number of arguments is used.
    switch (ea.argt & (EX_EXTRA | EX_NOSPC | EX_NEEDARG)) {
    case EX_EXTRA | EX_NOSPC | EX_NEEDARG:
      argc_valid = args.size == 1;
      break;
    case EX_EXTRA | EX_NOSPC:
      argc_valid = args.size <= 1;
      break;
    case EX_EXTRA | EX_NEEDARG:
      argc_valid = args.size >= 1;
      break;
    case EX_EXTRA:
      argc_valid = true;
      break;
    default:
      argc_valid = args.size == 0;
      break;
    }

    if (!argc_valid) {
      VALIDATION_ERROR("Incorrect number of arguments supplied");
    }
  }

  // Simply pass the first argument (if it exists) as the arg pointer to `set_cmd_addr_type()`
  // since it only ever checks the first argument.
  set_cmd_addr_type(&ea, args.size > 0 ? args.items[0].data.string.data : NULL);

  if (HAS_KEY(cmd->range)) {
    if (!(ea.argt & EX_RANGE)) {
      VALIDATION_ERROR("Command cannot accept a range");
    } else if (cmd->range.type != kObjectTypeArray) {
      VALIDATION_ERROR("'range' must be an Array");
    } else if (cmd->range.data.array.size > 2) {
      VALIDATION_ERROR("'range' cannot contain more than two elements");
    }

    Array range = cmd->range.data.array;
    ea.addr_count = (int)range.size;

    for (size_t i = 0; i < range.size; i++) {
      Object elem = range.items[i];
      if (elem.type != kObjectTypeInteger || elem.data.integer < 0) {
        VALIDATION_ERROR("'range' element must be a non-negative Integer");
      }
    }

    if (range.size > 0) {
      ea.line1 = (linenr_T)range.items[0].data.integer;
      ea.line2 = (linenr_T)range.items[range.size - 1].data.integer;
    }

    if (invalid_range(&ea) != NULL) {
      VALIDATION_ERROR("Invalid range provided");
    }
  }
  if (ea.addr_count == 0) {
    if (ea.argt & EX_DFLALL) {
      set_cmd_dflall_range(&ea);  // Default range for range=%
    } else {
      ea.line1 = ea.line2 = get_cmd_default_range(&ea);  // Default range.

      if (ea.addr_type == ADDR_OTHER) {
        // Default is 1, not cursor.
        ea.line2 = 1;
      }
    }
  }

  if (HAS_KEY(cmd->count)) {
    if (!(ea.argt & EX_COUNT)) {
      VALIDATION_ERROR("Command cannot accept a count");
    } else if (cmd->count.type != kObjectTypeInteger || cmd->count.data.integer < 0) {
      VALIDATION_ERROR("'count' must be a non-negative Integer");
    }
    set_cmd_count(&ea, (linenr_T)cmd->count.data.integer, true);
  }

  if (HAS_KEY(cmd->reg)) {
    if (!(ea.argt & EX_REGSTR)) {
      VALIDATION_ERROR("Command cannot accept a register");
    } else if (cmd->reg.type != kObjectTypeString || cmd->reg.data.string.size != 1) {
      VALIDATION_ERROR("'reg' must be a single character");
    }
    char regname = cmd->reg.data.string.data[0];
    if (regname == '=') {
      VALIDATION_ERROR("Cannot use register \"=");
    } else if (!valid_yank_reg(regname, ea.cmdidx != CMD_put && !IS_USER_CMDIDX(ea.cmdidx))) {
      VALIDATION_ERROR("Invalid register: \"%c", regname);
    }
    ea.regname = (uint8_t)regname;
  }

  OBJ_TO_BOOL(ea.forceit, cmd->bang, false, "'bang'");
  if (ea.forceit && !(ea.argt & EX_BANG)) {
    VALIDATION_ERROR("Command cannot accept a bang");
  }

  if (HAS_KEY(cmd->magic)) {
    if (cmd->magic.type != kObjectTypeDictionary) {
      VALIDATION_ERROR("'magic' must be a Dictionary");
    }

    Dict(cmd_magic) magic = { 0 };
    if (!api_dict_to_keydict(&magic, KeyDict_cmd_magic_get_field,
                             cmd->magic.data.dictionary, err)) {
      goto end;
    }

    OBJ_TO_BOOL(cmdinfo.magic.file, magic.file, ea.argt & EX_XFILE, "'magic.file'");
    OBJ_TO_BOOL(cmdinfo.magic.bar, magic.bar, ea.argt & EX_TRLBAR, "'magic.bar'");
    if (cmdinfo.magic.file) {
      ea.argt |= EX_XFILE;
    } else {
      ea.argt &= ~EX_XFILE;
    }
  } else {
    cmdinfo.magic.file = ea.argt & EX_XFILE;
    cmdinfo.magic.bar = ea.argt & EX_TRLBAR;
  }

  if (HAS_KEY(cmd->mods)) {
    if (cmd->mods.type != kObjectTypeDictionary) {
      VALIDATION_ERROR("'mods' must be a Dictionary");
    }

    Dict(cmd_mods) mods = { 0 };
    if (!api_dict_to_keydict(&mods, KeyDict_cmd_mods_get_field, cmd->mods.data.dictionary, err)) {
      goto end;
    }

    if (HAS_KEY(mods.filter)) {
      if (mods.filter.type != kObjectTypeDictionary) {
        VALIDATION_ERROR("'mods.filter' must be a Dictionary");
      }

      Dict(cmd_mods_filter) filter = { 0 };

      if (!api_dict_to_keydict(&filter, KeyDict_cmd_mods_filter_get_field,
                               mods.filter.data.dictionary, err)) {
        goto end;
      }

      if (HAS_KEY(filter.pattern)) {
        if (filter.pattern.type != kObjectTypeString) {
          VALIDATION_ERROR("'mods.filter.pattern' must be a String");
        }

        OBJ_TO_BOOL(cmdinfo.cmdmod.cmod_filter_force, filter.force, false, "'mods.filter.force'");

        // "filter! // is not no-op, so add a filter if either the pattern is non-empty or if filter
        // is inverted.
        if (*filter.pattern.data.string.data != NUL || cmdinfo.cmdmod.cmod_filter_force) {
          cmdinfo.cmdmod.cmod_filter_pat = string_to_cstr(filter.pattern.data.string);
          cmdinfo.cmdmod.cmod_filter_regmatch.regprog = vim_regcomp(cmdinfo.cmdmod.cmod_filter_pat,
                                                                    RE_MAGIC);
        }
      }
    }

    if (HAS_KEY(mods.tab)) {
      if (mods.tab.type != kObjectTypeInteger) {
        VALIDATION_ERROR("'mods.tab' must be an Integer");
      } else if ((int)mods.tab.data.integer >= 0) {
        // Silently ignore negative integers to allow mods.tab to be set to -1.
        cmdinfo.cmdmod.cmod_tab = (int)mods.tab.data.integer + 1;
      }
    }

    if (HAS_KEY(mods.verbose)) {
      if (mods.verbose.type != kObjectTypeInteger) {
        VALIDATION_ERROR("'mods.verbose' must be an Integer");
      } else if ((int)mods.verbose.data.integer >= 0) {
        // Silently ignore negative integers to allow mods.verbose to be set to -1.
        cmdinfo.cmdmod.cmod_verbose = (int)mods.verbose.data.integer + 1;
      }
    }

    bool vertical;
    OBJ_TO_BOOL(vertical, mods.vertical, false, "'mods.vertical'");
    cmdinfo.cmdmod.cmod_split |= (vertical ? WSP_VERT : 0);

    bool horizontal;
    OBJ_TO_BOOL(horizontal, mods.horizontal, false, "'mods.horizontal'");
    cmdinfo.cmdmod.cmod_split |= (horizontal ? WSP_HOR : 0);

    if (HAS_KEY(mods.split)) {
      if (mods.split.type != kObjectTypeString) {
        VALIDATION_ERROR("'mods.split' must be a String");
      }

      if (*mods.split.data.string.data == NUL) {
        // Empty string, do nothing.
      } else if (strcmp(mods.split.data.string.data, "aboveleft") == 0
                 || strcmp(mods.split.data.string.data, "leftabove") == 0) {
        cmdinfo.cmdmod.cmod_split |= WSP_ABOVE;
      } else if (strcmp(mods.split.data.string.data, "belowright") == 0
                 || strcmp(mods.split.data.string.data, "rightbelow") == 0) {
        cmdinfo.cmdmod.cmod_split |= WSP_BELOW;
      } else if (strcmp(mods.split.data.string.data, "topleft") == 0) {
        cmdinfo.cmdmod.cmod_split |= WSP_TOP;
      } else if (strcmp(mods.split.data.string.data, "botright") == 0) {
        cmdinfo.cmdmod.cmod_split |= WSP_BOT;
      } else {
        VALIDATION_ERROR("Invalid value for 'mods.split'");
      }
    }

    OBJ_TO_CMOD_FLAG(CMOD_SILENT, mods.silent, false, "'mods.silent'");
    OBJ_TO_CMOD_FLAG(CMOD_ERRSILENT, mods.emsg_silent, false, "'mods.emsg_silent'");
    OBJ_TO_CMOD_FLAG(CMOD_UNSILENT, mods.unsilent, false, "'mods.unsilent'");
    OBJ_TO_CMOD_FLAG(CMOD_SANDBOX, mods.sandbox, false, "'mods.sandbox'");
    OBJ_TO_CMOD_FLAG(CMOD_NOAUTOCMD, mods.noautocmd, false, "'mods.noautocmd'");
    OBJ_TO_CMOD_FLAG(CMOD_BROWSE, mods.browse, false, "'mods.browse'");
    OBJ_TO_CMOD_FLAG(CMOD_CONFIRM, mods.confirm, false, "'mods.confirm'");
    OBJ_TO_CMOD_FLAG(CMOD_HIDE, mods.hide, false, "'mods.hide'");
    OBJ_TO_CMOD_FLAG(CMOD_KEEPALT, mods.keepalt, false, "'mods.keepalt'");
    OBJ_TO_CMOD_FLAG(CMOD_KEEPJUMPS, mods.keepjumps, false, "'mods.keepjumps'");
    OBJ_TO_CMOD_FLAG(CMOD_KEEPMARKS, mods.keepmarks, false, "'mods.keepmarks'");
    OBJ_TO_CMOD_FLAG(CMOD_KEEPPATTERNS, mods.keeppatterns, false, "'mods.keeppatterns'");
    OBJ_TO_CMOD_FLAG(CMOD_LOCKMARKS, mods.lockmarks, false, "'mods.lockmarks'");
    OBJ_TO_CMOD_FLAG(CMOD_NOSWAPFILE, mods.noswapfile, false, "'mods.noswapfile'");

    if (cmdinfo.cmdmod.cmod_flags & CMOD_ERRSILENT) {
      // CMOD_ERRSILENT must imply CMOD_SILENT, otherwise apply_cmdmod() and undo_cmdmod() won't
      // work properly.
      cmdinfo.cmdmod.cmod_flags |= CMOD_SILENT;
    }

    if ((cmdinfo.cmdmod.cmod_flags & CMOD_SANDBOX) && !(ea.argt & EX_SBOXOK)) {
      VALIDATION_ERROR("Command cannot be run in sandbox");
    }
  }

  // Finally, build the command line string that will be stored inside ea.cmdlinep.
  // This also sets the values of ea.cmd, ea.arg, ea.args and ea.arglens.
  build_cmdline_str(&cmdline, &ea, &cmdinfo, args);
  ea.cmdlinep = &cmdline;

  garray_T capture_local;
  const int save_msg_silent = msg_silent;
  garray_T * const save_capture_ga = capture_ga;
  const int save_msg_col = msg_col;

  if (output) {
    ga_init(&capture_local, 1, 80);
    capture_ga = &capture_local;
  }

  TRY_WRAP({
    try_start();
    if (output) {
      msg_silent++;
      msg_col = 0;  // prevent leading spaces
    }

    WITH_SCRIPT_CONTEXT(channel_id, {
      execute_cmd(&ea, &cmdinfo, false);
    });

    if (output) {
      capture_ga = save_capture_ga;
      msg_silent = save_msg_silent;
      // Put msg_col back where it was, since nothing should have been written.
      msg_col = save_msg_col;
    }

    try_end(err);
  });

  if (ERROR_SET(err)) {
    goto clear_ga;
  }

  if (output && capture_local.ga_len > 1) {
    retv = (String){
      .data = capture_local.ga_data,
      .size = (size_t)capture_local.ga_len,
    };
    // redir usually (except :echon) prepends a newline.
    if (retv.data[0] == '\n') {
      memmove(retv.data, retv.data + 1, retv.size - 1);
      retv.data[retv.size - 1] = '\0';
      retv.size = retv.size - 1;
    }
    goto end;
  }
clear_ga:
  if (output) {
    ga_clear(&capture_local);
  }
end:
  api_free_array(args);
  xfree(cmdline);
  xfree(cmdname);
  xfree(ea.args);
  xfree(ea.arglens);

  return retv;

#undef OBJ_TO_BOOL
#undef OBJ_TO_CMOD_FLAG
#undef VALIDATION_ERROR
}

/// Check if a string contains only whitespace characters.
static bool string_iswhite(String str)
{
  for (size_t i = 0; i < str.size; i++) {
    if (!ascii_iswhite(str.data[i])) {
      // Found a non-whitespace character
      return false;
    } else if (str.data[i] == NUL) {
      // Terminate at first occurrence of a NUL character
      break;
    }
  }
  return true;
}

/// Build cmdline string for command, used by `nvim_cmd()`.
static void build_cmdline_str(char **cmdlinep, exarg_T *eap, CmdParseInfo *cmdinfo,
                              ArrayOf(String) args)
{
  size_t argc = args.size;
  StringBuilder cmdline = KV_INITIAL_VALUE;
  kv_resize(cmdline, 32);  // Make it big enough to handle most typical commands

  // Add command modifiers
  if (cmdinfo->cmdmod.cmod_tab != 0) {
    kv_printf(cmdline, "%dtab ", cmdinfo->cmdmod.cmod_tab - 1);
  }
  if (cmdinfo->cmdmod.cmod_verbose > 0) {
    kv_printf(cmdline, "%dverbose ", cmdinfo->cmdmod.cmod_verbose - 1);
  }

  if (cmdinfo->cmdmod.cmod_flags & CMOD_ERRSILENT) {
    kv_concat(cmdline, "silent! ");
  } else if (cmdinfo->cmdmod.cmod_flags & CMOD_SILENT) {
    kv_concat(cmdline, "silent ");
  }

  if (cmdinfo->cmdmod.cmod_flags & CMOD_UNSILENT) {
    kv_concat(cmdline, "unsilent ");
  }

  switch (cmdinfo->cmdmod.cmod_split & (WSP_ABOVE | WSP_BELOW | WSP_TOP | WSP_BOT)) {
  case WSP_ABOVE:
    kv_concat(cmdline, "aboveleft ");
    break;
  case WSP_BELOW:
    kv_concat(cmdline, "belowright ");
    break;
  case WSP_TOP:
    kv_concat(cmdline, "topleft ");
    break;
  case WSP_BOT:
    kv_concat(cmdline, "botright ");
    break;
  default:
    break;
  }

#define CMDLINE_APPEND_IF(cond, str) \
  do { \
    if (cond) { \
      kv_concat(cmdline, str); \
    } \
  } while (0)

  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_split & WSP_VERT, "vertical ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_split & WSP_HOR, "horizontal ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_SANDBOX, "sandbox ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_NOAUTOCMD, "noautocmd ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_BROWSE, "browse ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_CONFIRM, "confirm ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_HIDE, "hide ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_KEEPALT, "keepalt ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_KEEPJUMPS, "keepjumps ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_KEEPMARKS, "keepmarks ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_KEEPPATTERNS, "keeppatterns ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_LOCKMARKS, "lockmarks ");
  CMDLINE_APPEND_IF(cmdinfo->cmdmod.cmod_flags & CMOD_NOSWAPFILE, "noswapfile ");
#undef CMDLINE_APPEND_IF

  // Command range / count.
  if (eap->argt & EX_RANGE) {
    if (eap->addr_count == 1) {
      kv_printf(cmdline, "%" PRIdLINENR, eap->line2);
    } else if (eap->addr_count > 1) {
      kv_printf(cmdline, "%" PRIdLINENR ",%" PRIdLINENR, eap->line1, eap->line2);
      eap->addr_count = 2;  // Make sure address count is not greater than 2
    }
  }

  // Keep the index of the position where command name starts, so eap->cmd can point to it.
  size_t cmdname_idx = cmdline.size;
  kv_concat(cmdline, eap->cmd);

  // Command bang.
  if (eap->argt & EX_BANG && eap->forceit) {
    kv_concat(cmdline, "!");
  }

  // Command register.
  if (eap->argt & EX_REGSTR && eap->regname) {
    kv_printf(cmdline, " %c", eap->regname);
  }

  eap->argc = argc;
  eap->arglens = eap->argc > 0 ? xcalloc(argc, sizeof(size_t)) : NULL;
  size_t argstart_idx = cmdline.size;
  for (size_t i = 0; i < argc; i++) {
    String s = args.items[i].data.string;
    eap->arglens[i] = s.size;
    kv_concat(cmdline, " ");
    kv_concat_len(cmdline, s.data, s.size);
  }

  // Done appending to cmdline, ensure it is NUL terminated
  kv_push(cmdline, NUL);

  // Now that all the arguments are appended, use the command index and argument indices to set the
  // values of eap->cmd, eap->arg and eap->args.
  eap->cmd = cmdline.items + cmdname_idx;
  eap->args = eap->argc > 0 ? xcalloc(argc, sizeof(char *)) : NULL;
  size_t offset = argstart_idx;
  for (size_t i = 0; i < argc; i++) {
    offset++;  // Account for space
    eap->args[i] = cmdline.items + offset;
    offset += eap->arglens[i];
  }
  // If there isn't an argument, make eap->arg point to end of cmdline.
  eap->arg = argc > 0 ? eap->args[0] :
             cmdline.items + cmdline.size - 1;  // Subtract 1 to account for NUL

  // Finally, make cmdlinep point to the cmdline string.
  *cmdlinep = cmdline.items;

  // Replace, :make and :grep with 'makeprg' and 'grepprg'.
  char *p = replace_makeprg(eap, eap->arg, cmdlinep);
  if (p != eap->arg) {
    // If replace_makeprg() modified the cmdline string, correct the eap->arg pointer.
    eap->arg = p;
    // This cannot be a user command, so eap->args will not be used.
    XFREE_CLEAR(eap->args);
    XFREE_CLEAR(eap->arglens);
    eap->argc = 0;
  }
}

/// Create a new user command |user-commands|
///
/// {name} is the name of the new command. The name must begin with an uppercase letter.
///
/// {command} is the replacement text or Lua function to execute.
///
/// Example:
/// <pre>vim
///    :call nvim_create_user_command('SayHello', 'echo "Hello world!"', {})
///    :SayHello
///    Hello world!
/// </pre>
///
/// @param  name    Name of the new user command. Must begin with an uppercase letter.
/// @param  command Replacement command to execute when this user command is executed. When called
///                 from Lua, the command can also be a Lua function. The function is called with a
///                 single table argument that contains the following keys:
///                 - name: (string) Command name
///                 - args: (string) The args passed to the command, if any |<args>|
///                 - fargs: (table) The args split by unescaped whitespace (when more than one
///                 argument is allowed), if any |<f-args>|
///                 - bang: (boolean) "true" if the command was executed with a ! modifier |<bang>|
///                 - line1: (number) The starting line of the command range |<line1>|
///                 - line2: (number) The final line of the command range |<line2>|
///                 - range: (number) The number of items in the command range: 0, 1, or 2 |<range>|
///                 - count: (number) Any count supplied |<count>|
///                 - reg: (string) The optional register, if specified |<reg>|
///                 - mods: (string) Command modifiers, if any |<mods>|
///                 - smods: (table) Command modifiers in a structured format. Has the same
///                 structure as the "mods" key of |nvim_parse_cmd()|.
/// @param  opts    Optional command attributes. See |command-attributes| for more details. To use
///                 boolean attributes (such as |:command-bang| or |:command-bar|) set the value to
///                 "true". In addition to the string options listed in |:command-complete|, the
///                 "complete" key also accepts a Lua function which works like the "customlist"
///                 completion mode |:command-completion-customlist|. Additional parameters:
///                 - desc: (string) Used for listing the command when a Lua function is used for
///                                  {command}.
///                 - force: (boolean, default true) Override any previous definition.
///                 - preview: (function) Preview callback for 'inccommand' |:command-preview|
/// @param[out] err Error details, if any.
void nvim_create_user_command(String name, Object command, Dict(user_command) *opts, Error *err)
  FUNC_API_SINCE(9)
{
  create_user_command(name, command, opts, 0, err);
}

/// Delete a user-defined command.
///
/// @param  name    Name of the command to delete.
/// @param[out] err Error details, if any.
void nvim_del_user_command(String name, Error *err)
  FUNC_API_SINCE(9)
{
  nvim_buf_del_user_command(-1, name, err);
}

/// Create a new user command |user-commands| in the given buffer.
///
/// @param  buffer  Buffer handle, or 0 for current buffer.
/// @param[out] err Error details, if any.
/// @see nvim_create_user_command
void nvim_buf_create_user_command(Buffer buffer, String name, Object command,
                                  Dict(user_command) *opts, Error *err)
  FUNC_API_SINCE(9)
{
  buf_T *target_buf = find_buffer_by_handle(buffer, err);
  if (ERROR_SET(err)) {
    return;
  }

  buf_T *save_curbuf = curbuf;
  curbuf = target_buf;
  create_user_command(name, command, opts, UC_BUFFER, err);
  curbuf = save_curbuf;
}

/// Delete a buffer-local user-defined command.
///
/// Only commands created with |:command-buffer| or
/// |nvim_buf_create_user_command()| can be deleted with this function.
///
/// @param  buffer  Buffer handle, or 0 for current buffer.
/// @param  name    Name of the command to delete.
/// @param[out] err Error details, if any.
void nvim_buf_del_user_command(Buffer buffer, String name, Error *err)
  FUNC_API_SINCE(9)
{
  garray_T *gap;
  if (buffer == -1) {
    gap = &ucmds;
  } else {
    buf_T *buf = find_buffer_by_handle(buffer, err);
    gap = &buf->b_ucmds;
  }

  for (int i = 0; i < gap->ga_len; i++) {
    ucmd_T *cmd = USER_CMD_GA(gap, i);
    if (!strcmp(name.data, cmd->uc_name)) {
      free_ucmd(cmd);

      gap->ga_len -= 1;

      if (i < gap->ga_len) {
        memmove(cmd, cmd + 1, (size_t)(gap->ga_len - i) * sizeof(ucmd_T));
      }

      return;
    }
  }

  api_set_error(err, kErrorTypeException, "No such user-defined command: %s", name.data);
}

void create_user_command(String name, Object command, Dict(user_command) *opts, int flags,
                         Error *err)
{
  uint32_t argt = 0;
  int64_t def = -1;
  cmd_addr_T addr_type_arg = ADDR_NONE;
  int compl = EXPAND_NOTHING;
  char *compl_arg = NULL;
  const char *rep = NULL;
  LuaRef luaref = LUA_NOREF;
  LuaRef compl_luaref = LUA_NOREF;
  LuaRef preview_luaref = LUA_NOREF;

  if (!uc_validate_name(name.data)) {
    api_set_error(err, kErrorTypeValidation, "Invalid command name");
    goto err;
  }

  if (mb_islower(name.data[0])) {
    api_set_error(err, kErrorTypeValidation, "'name' must begin with an uppercase letter");
    goto err;
  }

  if (HAS_KEY(opts->range) && HAS_KEY(opts->count)) {
    api_set_error(err, kErrorTypeValidation, "'range' and 'count' are mutually exclusive");
    goto err;
  }

  if (opts->nargs.type == kObjectTypeInteger) {
    switch (opts->nargs.data.integer) {
    case 0:
      // Default value, nothing to do
      break;
    case 1:
      argt |= EX_EXTRA | EX_NOSPC | EX_NEEDARG;
      break;
    default:
      api_set_error(err, kErrorTypeValidation, "Invalid value for 'nargs'");
      goto err;
    }
  } else if (opts->nargs.type == kObjectTypeString) {
    if (opts->nargs.data.string.size > 1) {
      api_set_error(err, kErrorTypeValidation, "Invalid value for 'nargs'");
      goto err;
    }

    switch (opts->nargs.data.string.data[0]) {
    case '*':
      argt |= EX_EXTRA;
      break;
    case '?':
      argt |= EX_EXTRA | EX_NOSPC;
      break;
    case '+':
      argt |= EX_EXTRA | EX_NEEDARG;
      break;
    default:
      api_set_error(err, kErrorTypeValidation, "Invalid value for 'nargs'");
      goto err;
    }
  } else if (HAS_KEY(opts->nargs)) {
    api_set_error(err, kErrorTypeValidation, "Invalid value for 'nargs'");
    goto err;
  }

  if (HAS_KEY(opts->complete) && !argt) {
    api_set_error(err, kErrorTypeValidation, "'complete' used without 'nargs'");
    goto err;
  }

  if (opts->range.type == kObjectTypeBoolean) {
    if (opts->range.data.boolean) {
      argt |= EX_RANGE;
      addr_type_arg = ADDR_LINES;
    }
  } else if (opts->range.type == kObjectTypeString) {
    if (opts->range.data.string.data[0] == '%' && opts->range.data.string.size == 1) {
      argt |= EX_RANGE | EX_DFLALL;
      addr_type_arg = ADDR_LINES;
    } else {
      api_set_error(err, kErrorTypeValidation, "Invalid value for 'range'");
      goto err;
    }
  } else if (opts->range.type == kObjectTypeInteger) {
    argt |= EX_RANGE | EX_ZEROR;
    def = opts->range.data.integer;
    addr_type_arg = ADDR_LINES;
  } else if (HAS_KEY(opts->range)) {
    api_set_error(err, kErrorTypeValidation, "Invalid value for 'range'");
    goto err;
  }

  if (opts->count.type == kObjectTypeBoolean) {
    if (opts->count.data.boolean) {
      argt |= EX_COUNT | EX_ZEROR | EX_RANGE;
      addr_type_arg = ADDR_OTHER;
      def = 0;
    }
  } else if (opts->count.type == kObjectTypeInteger) {
    argt |= EX_COUNT | EX_ZEROR | EX_RANGE;
    addr_type_arg = ADDR_OTHER;
    def = opts->count.data.integer;
  } else if (HAS_KEY(opts->count)) {
    api_set_error(err, kErrorTypeValidation, "Invalid value for 'count'");
    goto err;
  }

  if (opts->addr.type == kObjectTypeString) {
    if (parse_addr_type_arg(opts->addr.data.string.data, (int)opts->addr.data.string.size,
                            &addr_type_arg) != OK) {
      api_set_error(err, kErrorTypeValidation, "Invalid value for 'addr'");
      goto err;
    }

    if (addr_type_arg != ADDR_LINES) {
      argt |= EX_ZEROR;
    }
  } else if (HAS_KEY(opts->addr)) {
    api_set_error(err, kErrorTypeValidation, "Invalid value for 'addr'");
    goto err;
  }

  if (api_object_to_bool(opts->bang, "bang", false, err)) {
    argt |= EX_BANG;
  } else if (ERROR_SET(err)) {
    goto err;
  }

  if (api_object_to_bool(opts->bar, "bar", false, err)) {
    argt |= EX_TRLBAR;
  } else if (ERROR_SET(err)) {
    goto err;
  }

  if (api_object_to_bool(opts->register_, "register", false, err)) {
    argt |= EX_REGSTR;
  } else if (ERROR_SET(err)) {
    goto err;
  }

  if (api_object_to_bool(opts->keepscript, "keepscript", false, err)) {
    argt |= EX_KEEPSCRIPT;
  } else if (ERROR_SET(err)) {
    goto err;
  }

  bool force = api_object_to_bool(opts->force, "force", true, err);
  if (ERROR_SET(err)) {
    goto err;
  }

  if (opts->complete.type == kObjectTypeLuaRef) {
    compl = EXPAND_USER_LUA;
    compl_luaref = api_new_luaref(opts->complete.data.luaref);
  } else if (opts->complete.type == kObjectTypeString) {
    if (parse_compl_arg(opts->complete.data.string.data,
                        (int)opts->complete.data.string.size, &compl, &argt,
                        &compl_arg) != OK) {
      api_set_error(err, kErrorTypeValidation, "Invalid value for 'complete'");
      goto err;
    }
  } else if (HAS_KEY(opts->complete)) {
    api_set_error(err, kErrorTypeValidation, "Invalid value for 'complete'");
    goto err;
  }

  if (opts->preview.type == kObjectTypeLuaRef) {
    argt |= EX_PREVIEW;
    preview_luaref = api_new_luaref(opts->preview.data.luaref);
  } else if (HAS_KEY(opts->preview)) {
    api_set_error(err, kErrorTypeValidation, "Invalid value for 'preview'");
    goto err;
  }

  switch (command.type) {
  case kObjectTypeLuaRef:
    luaref = api_new_luaref(command.data.luaref);
    if (opts->desc.type == kObjectTypeString) {
      rep = opts->desc.data.string.data;
    } else {
      rep = "";
    }
    break;
  case kObjectTypeString:
    rep = command.data.string.data;
    break;
  default:
    api_set_error(err, kErrorTypeValidation, "'command' must be a string or Lua function");
    goto err;
  }

  if (uc_add_command(name.data, name.size, rep, argt, def, flags, compl, compl_arg, compl_luaref,
                     preview_luaref, addr_type_arg, luaref, force) != OK) {
    api_set_error(err, kErrorTypeException, "Failed to create user command");
    // Do not goto err, since uc_add_command now owns luaref, compl_luaref, and compl_arg
  }

  return;

err:
  NLUA_CLEAR_REF(luaref);
  NLUA_CLEAR_REF(compl_luaref);
  xfree(compl_arg);
}
/// Gets a map of global (non-buffer-local) Ex commands.
///
/// Currently only |user-commands| are supported, not builtin Ex commands.
///
/// @param  opts  Optional parameters. Currently only supports
///               {"builtin":false}
/// @param[out]  err   Error details, if any.
///
/// @returns Map of maps describing commands.
Dictionary nvim_get_commands(Dict(get_commands) *opts, Error *err)
  FUNC_API_SINCE(4)
{
  return nvim_buf_get_commands(-1, opts, err);
}

/// Gets a map of buffer-local |user-commands|.
///
/// @param  buffer  Buffer handle, or 0 for current buffer
/// @param  opts  Optional parameters. Currently not used.
/// @param[out]  err   Error details, if any.
///
/// @returns Map of maps describing commands.
Dictionary nvim_buf_get_commands(Buffer buffer, Dict(get_commands) *opts, Error *err)
  FUNC_API_SINCE(4)
{
  bool global = (buffer == -1);
  bool builtin = api_object_to_bool(opts->builtin, "builtin", false, err);
  if (ERROR_SET(err)) {
    return (Dictionary)ARRAY_DICT_INIT;
  }

  if (global) {
    if (builtin) {
      api_set_error(err, kErrorTypeValidation, "builtin=true not implemented");
      return (Dictionary)ARRAY_DICT_INIT;
    }
    return commands_array(NULL);
  }

  buf_T *buf = find_buffer_by_handle(buffer, err);
  if (builtin || !buf) {
    return (Dictionary)ARRAY_DICT_INIT;
  }
  return commands_array(buf);
}
