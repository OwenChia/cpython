#include "Python.h"
#include "pycore_coreconfig.h"
#include "pycore_getopt.h"
#include "pycore_pystate.h"   /* _PyRuntime_Initialize() */
#include <locale.h>       /* setlocale() */


#define DECODE_LOCALE_ERR(NAME, LEN) \
    (((LEN) == -2) \
     ? _Py_INIT_ERR("cannot decode " NAME) \
     : _Py_INIT_NO_MEMORY())


/* --- File system encoding/errors -------------------------------- */

/* The filesystem encoding is chosen by config_init_fs_encoding(),
   see also initfsencoding().

   Py_FileSystemDefaultEncoding and Py_FileSystemDefaultEncodeErrors
   are encoded to UTF-8. */
const char *Py_FileSystemDefaultEncoding = NULL;
int Py_HasFileSystemDefaultEncoding = 0;
const char *Py_FileSystemDefaultEncodeErrors = NULL;
int _Py_HasFileSystemDefaultEncodeErrors = 0;

void
_Py_ClearFileSystemEncoding(void)
{
    if (!Py_HasFileSystemDefaultEncoding && Py_FileSystemDefaultEncoding) {
        PyMem_RawFree((char*)Py_FileSystemDefaultEncoding);
        Py_FileSystemDefaultEncoding = NULL;
    }
    if (!_Py_HasFileSystemDefaultEncodeErrors && Py_FileSystemDefaultEncodeErrors) {
        PyMem_RawFree((char*)Py_FileSystemDefaultEncodeErrors);
        Py_FileSystemDefaultEncodeErrors = NULL;
    }
}


/* Set Py_FileSystemDefaultEncoding and Py_FileSystemDefaultEncodeErrors
   global configuration variables. */
int
_Py_SetFileSystemEncoding(const char *encoding, const char *errors)
{
    char *encoding2 = _PyMem_RawStrdup(encoding);
    if (encoding2 == NULL) {
        return -1;
    }

    char *errors2 = _PyMem_RawStrdup(errors);
    if (errors2 == NULL) {
        PyMem_RawFree(encoding2);
        return -1;
    }

    _Py_ClearFileSystemEncoding();

    Py_FileSystemDefaultEncoding = encoding2;
    Py_HasFileSystemDefaultEncoding = 0;

    Py_FileSystemDefaultEncodeErrors = errors2;
    _Py_HasFileSystemDefaultEncodeErrors = 0;
    return 0;
}


/* --- _PyArgv ---------------------------------------------------- */

/* Decode bytes_argv using Py_DecodeLocale() */
_PyInitError
_PyArgv_AsWstrList(const _PyArgv *args, _PyWstrList *list)
{
    _PyWstrList wargv = _PyWstrList_INIT;
    if (args->use_bytes_argv) {
        size_t size = sizeof(wchar_t*) * args->argc;
        wargv.items = (wchar_t **)PyMem_RawMalloc(size);
        if (wargv.items == NULL) {
            return _Py_INIT_NO_MEMORY();
        }

        for (Py_ssize_t i = 0; i < args->argc; i++) {
            size_t len;
            wchar_t *arg = Py_DecodeLocale(args->bytes_argv[i], &len);
            if (arg == NULL) {
                _PyWstrList_Clear(&wargv);
                return DECODE_LOCALE_ERR("command line arguments",
                                         (Py_ssize_t)len);
            }
            wargv.items[i] = arg;
            wargv.length++;
        }

        _PyWstrList_Clear(list);
        *list = wargv;
    }
    else {
        wargv.length = args->argc;
        wargv.items = (wchar_t **)args->wchar_argv;
        if (_PyWstrList_Copy(list, &wargv) < 0) {
            return _Py_INIT_NO_MEMORY();
        }
    }
    return _Py_INIT_OK();
}


/* --- _PyPreCmdline ------------------------------------------------- */

void
_PyPreCmdline_Clear(_PyPreCmdline *cmdline)
{
    _PyWstrList_Clear(&cmdline->argv);
    _PyWstrList_Clear(&cmdline->xoptions);
}


_PyInitError
_PyPreCmdline_SetArgv(_PyPreCmdline *cmdline, const _PyArgv *args)
{
    return _PyArgv_AsWstrList(args, &cmdline->argv);
}


static void
_PyPreCmdline_GetPreConfig(_PyPreCmdline *cmdline, const _PyPreConfig *config)
{
#define COPY_ATTR(ATTR) \
    if (config->ATTR != -1) { \
        cmdline->ATTR = config->ATTR; \
    }

    COPY_ATTR(isolated);
    COPY_ATTR(use_environment);
    COPY_ATTR(dev_mode);

#undef COPY_ATTR
}


static void
_PyPreCmdline_SetPreConfig(const _PyPreCmdline *cmdline, _PyPreConfig *config)
{
#define COPY_ATTR(ATTR) \
    config->ATTR = cmdline->ATTR

    COPY_ATTR(isolated);
    COPY_ATTR(use_environment);
    COPY_ATTR(dev_mode);

#undef COPY_ATTR
}


int
_PyPreCmdline_SetCoreConfig(const _PyPreCmdline *cmdline, _PyCoreConfig *config)
{
#define COPY_ATTR(ATTR) \
    config->ATTR = cmdline->ATTR

    if (_PyWstrList_Extend(&config->xoptions, &cmdline->xoptions) < 0) {
        return -1;
    }

    COPY_ATTR(isolated);
    COPY_ATTR(use_environment);
    COPY_ATTR(dev_mode);
    return 0;

#undef COPY_ATTR
}


/* Parse the command line arguments */
static _PyInitError
precmdline_parse_cmdline(_PyPreCmdline *cmdline)
{
    const _PyWstrList *argv = &cmdline->argv;

    _PyOS_ResetGetOpt();
    /* Don't log parsing errors into stderr here: _PyCoreConfig_Read()
       is responsible for that */
    _PyOS_opterr = 0;
    do {
        int longindex = -1;
        int c = _PyOS_GetOpt(argv->length, argv->items, &longindex);

        if (c == EOF || c == 'c' || c == 'm') {
            break;
        }

        switch (c) {
        case 'E':
            cmdline->use_environment = 0;
            break;

        case 'I':
            cmdline->isolated = 1;
            break;

        case 'X':
        {
            if (_PyWstrList_Append(&cmdline->xoptions, _PyOS_optarg) < 0) {
                return _Py_INIT_NO_MEMORY();
            }
            break;
        }

        default:
            /* ignore other argument:
               handled by _PyCoreConfig_Read() */
            break;
        }
    } while (1);

    return _Py_INIT_OK();
}


_PyInitError
_PyPreCmdline_Read(_PyPreCmdline *cmdline, const _PyPreConfig *preconfig)
{
    _PyPreCmdline_GetPreConfig(cmdline, preconfig);

    if (preconfig->parse_argv) {
        _PyInitError err = precmdline_parse_cmdline(cmdline);
        if (_Py_INIT_FAILED(err)) {
            return err;
        }
    }

    /* isolated, use_environment */
    if (cmdline->isolated < 0) {
        cmdline->isolated = 0;
    }
    if (cmdline->isolated > 0) {
        cmdline->use_environment = 0;
    }
    if (cmdline->use_environment < 0) {
        cmdline->use_environment = 0;
    }

    /* dev_mode */
    if ((cmdline->dev_mode < 0)
        && (_Py_get_xoption(&cmdline->xoptions, L"dev")
            || _Py_GetEnv(cmdline->use_environment, "PYTHONDEVMODE")))
    {
        cmdline->dev_mode = 1;
    }
    if (cmdline->dev_mode < 0) {
        cmdline->dev_mode = 0;
    }

    assert(cmdline->use_environment >= 0);
    assert(cmdline->isolated >= 0);
    assert(cmdline->dev_mode >= 0);

    return _Py_INIT_OK();
}


/* --- _PyPreConfig ----------------------------------------------- */


void
_PyPreConfig_Init(_PyPreConfig *config)
{
    memset(config, 0, sizeof(*config));

    config->_config_version = _Py_CONFIG_VERSION;
    config->parse_argv = 0;
    config->isolated = -1;
    config->use_environment = -1;
    config->configure_locale = 1;
    config->utf8_mode = -2;
    config->dev_mode = -1;
    config->allocator = PYMEM_ALLOCATOR_NOT_SET;
#ifdef MS_WINDOWS
    config->legacy_windows_fs_encoding = -1;
#endif
}


void
_PyPreConfig_InitPythonConfig(_PyPreConfig *config)
{
    _PyPreConfig_Init(config);

    config->parse_argv = 1;
    /* Set to -1 to enable C locale coercion (PEP 538) and UTF-8 Mode (PEP 540)
       depending on the LC_CTYPE locale, PYTHONUTF8 and PYTHONCOERCECLOCALE
       environment variables. */
    config->coerce_c_locale = -1;
    config->coerce_c_locale_warn = -1;
    config->utf8_mode = -1;
}


void
_PyPreConfig_InitIsolatedConfig(_PyPreConfig *config)
{
    _PyPreConfig_Init(config);

    config->configure_locale = 0;
    config->isolated = 1;
    config->use_environment = 0;
    config->utf8_mode = 0;
    config->dev_mode = 0;
#ifdef MS_WINDOWS
    config->legacy_windows_fs_encoding = 0;
#endif
}


void
_PyPreConfig_InitFromPreConfig(_PyPreConfig *config,
                               const _PyPreConfig *config2)
{
    _PyPreConfig_Init(config);
    _PyPreConfig_Copy(config, config2);
}


void
_PyPreConfig_InitFromCoreConfig(_PyPreConfig *config,
                                const _PyCoreConfig *coreconfig)
{
    _PyCoreConfigInitEnum config_init = (_PyCoreConfigInitEnum)coreconfig->_config_init;
    switch (config_init) {
    case _PyCoreConfig_INIT_PYTHON:
        _PyPreConfig_InitPythonConfig(config);
        break;
    case _PyCoreConfig_INIT_ISOLATED:
        _PyPreConfig_InitIsolatedConfig(config);
        break;
    case _PyCoreConfig_INIT:
    default:
        _PyPreConfig_Init(config);
    }
    _PyPreConfig_GetCoreConfig(config, coreconfig);
}


void
_PyPreConfig_Copy(_PyPreConfig *config, const _PyPreConfig *config2)
{
#define COPY_ATTR(ATTR) config->ATTR = config2->ATTR

    COPY_ATTR(parse_argv);
    COPY_ATTR(isolated);
    COPY_ATTR(use_environment);
    COPY_ATTR(configure_locale);
    COPY_ATTR(dev_mode);
    COPY_ATTR(coerce_c_locale);
    COPY_ATTR(coerce_c_locale_warn);
    COPY_ATTR(utf8_mode);
    COPY_ATTR(allocator);
#ifdef MS_WINDOWS
    COPY_ATTR(legacy_windows_fs_encoding);
#endif

#undef COPY_ATTR
}


PyObject*
_PyPreConfig_AsDict(const _PyPreConfig *config)
{
    PyObject *dict;

    dict = PyDict_New();
    if (dict == NULL) {
        return NULL;
    }

#define SET_ITEM_INT(ATTR) \
        do { \
            PyObject *obj = PyLong_FromLong(config->ATTR); \
            if (obj == NULL) { \
                goto fail; \
            } \
            int res = PyDict_SetItemString(dict, #ATTR, obj); \
            Py_DECREF(obj); \
            if (res < 0) { \
                goto fail; \
            } \
        } while (0)

    SET_ITEM_INT(parse_argv);
    SET_ITEM_INT(isolated);
    SET_ITEM_INT(use_environment);
    SET_ITEM_INT(configure_locale);
    SET_ITEM_INT(coerce_c_locale);
    SET_ITEM_INT(coerce_c_locale_warn);
    SET_ITEM_INT(utf8_mode);
#ifdef MS_WINDOWS
    SET_ITEM_INT(legacy_windows_fs_encoding);
#endif
    SET_ITEM_INT(dev_mode);
    SET_ITEM_INT(allocator);
    return dict;

fail:
    Py_DECREF(dict);
    return NULL;

#undef SET_ITEM_INT
}


void
_PyPreConfig_GetCoreConfig(_PyPreConfig *config,
                            const _PyCoreConfig *core_config)
{
#define COPY_ATTR(ATTR) \
    if (core_config->ATTR != -1) { \
        config->ATTR = core_config->ATTR; \
    }

    COPY_ATTR(parse_argv);
    COPY_ATTR(isolated);
    COPY_ATTR(use_environment);
    COPY_ATTR(dev_mode);

#undef COPY_ATTR
}


static void
_PyPreConfig_GetGlobalConfig(_PyPreConfig *config)
{
#define COPY_FLAG(ATTR, VALUE) \
    if (config->ATTR < 0) { \
        config->ATTR = VALUE; \
    }
#define COPY_NOT_FLAG(ATTR, VALUE) \
    if (config->ATTR < 0) { \
        config->ATTR = !(VALUE); \
    }

    COPY_FLAG(isolated, Py_IsolatedFlag);
    COPY_NOT_FLAG(use_environment, Py_IgnoreEnvironmentFlag);
#ifdef MS_WINDOWS
    COPY_FLAG(legacy_windows_fs_encoding, Py_LegacyWindowsFSEncodingFlag);
#endif
    if (config->utf8_mode == -2) {
        config->utf8_mode = Py_UTF8Mode;
    }

#undef COPY_FLAG
#undef COPY_NOT_FLAG
}


static void
_PyPreConfig_SetGlobalConfig(const _PyPreConfig *config)
{
#define COPY_FLAG(ATTR, VAR) \
    if (config->ATTR >= 0) { \
        VAR = config->ATTR; \
    }
#define COPY_NOT_FLAG(ATTR, VAR) \
    if (config->ATTR >= 0) { \
        VAR = !config->ATTR; \
    }

    COPY_FLAG(isolated, Py_IsolatedFlag);
    COPY_NOT_FLAG(use_environment, Py_IgnoreEnvironmentFlag);
#ifdef MS_WINDOWS
    COPY_FLAG(legacy_windows_fs_encoding, Py_LegacyWindowsFSEncodingFlag);
#endif
    COPY_FLAG(utf8_mode, Py_UTF8Mode);

#undef COPY_FLAG
#undef COPY_NOT_FLAG
}


const char*
_Py_GetEnv(int use_environment, const char *name)
{
    assert(use_environment >= 0);

    if (!use_environment) {
        return NULL;
    }

    const char *var = getenv(name);
    if (var && var[0] != '\0') {
        return var;
    }
    else {
        return NULL;
    }
}


int
_Py_str_to_int(const char *str, int *result)
{
    const char *endptr = str;
    errno = 0;
    long value = strtol(str, (char **)&endptr, 10);
    if (*endptr != '\0' || errno == ERANGE) {
        return -1;
    }
    if (value < INT_MIN || value > INT_MAX) {
        return -1;
    }

    *result = (int)value;
    return 0;
}


void
_Py_get_env_flag(int use_environment, int *flag, const char *name)
{
    const char *var = _Py_GetEnv(use_environment, name);
    if (!var) {
        return;
    }
    int value;
    if (_Py_str_to_int(var, &value) < 0 || value < 0) {
        /* PYTHONDEBUG=text and PYTHONDEBUG=-2 behave as PYTHONDEBUG=1 */
        value = 1;
    }
    if (*flag < value) {
        *flag = value;
    }
}


const wchar_t*
_Py_get_xoption(const _PyWstrList *xoptions, const wchar_t *name)
{
    for (Py_ssize_t i=0; i < xoptions->length; i++) {
        const wchar_t *option = xoptions->items[i];
        size_t len;
        wchar_t *sep = wcschr(option, L'=');
        if (sep != NULL) {
            len = (sep - option);
        }
        else {
            len = wcslen(option);
        }
        if (wcsncmp(option, name, len) == 0 && name[len] == L'\0') {
            return option;
        }
    }
    return NULL;
}


static _PyInitError
preconfig_init_utf8_mode(_PyPreConfig *config, const _PyPreCmdline *cmdline)
{
#ifdef MS_WINDOWS
    if (config->legacy_windows_fs_encoding) {
        config->utf8_mode = 0;
    }
#endif

    if (config->utf8_mode >= 0) {
        return _Py_INIT_OK();
    }

    const wchar_t *xopt;
    if (cmdline) {
        xopt = _Py_get_xoption(&cmdline->xoptions, L"utf8");
    }
    else {
        xopt = NULL;
    }
    if (xopt) {
        wchar_t *sep = wcschr(xopt, L'=');
        if (sep) {
            xopt = sep + 1;
            if (wcscmp(xopt, L"1") == 0) {
                config->utf8_mode = 1;
            }
            else if (wcscmp(xopt, L"0") == 0) {
                config->utf8_mode = 0;
            }
            else {
                return _Py_INIT_ERR("invalid -X utf8 option value");
            }
        }
        else {
            config->utf8_mode = 1;
        }
        return _Py_INIT_OK();
    }

    const char *opt = _Py_GetEnv(config->use_environment, "PYTHONUTF8");
    if (opt) {
        if (strcmp(opt, "1") == 0) {
            config->utf8_mode = 1;
        }
        else if (strcmp(opt, "0") == 0) {
            config->utf8_mode = 0;
        }
        else {
            return _Py_INIT_ERR("invalid PYTHONUTF8 environment "
                                "variable value");
        }
        return _Py_INIT_OK();
    }


#ifndef MS_WINDOWS
    if (config->utf8_mode < 0) {
        /* The C locale and the POSIX locale enable the UTF-8 Mode (PEP 540) */
        const char *ctype_loc = setlocale(LC_CTYPE, NULL);
        if (ctype_loc != NULL
           && (strcmp(ctype_loc, "C") == 0
               || strcmp(ctype_loc, "POSIX") == 0))
        {
            config->utf8_mode = 1;
        }
    }
#endif

    if (config->utf8_mode < 0) {
        config->utf8_mode = 0;
    }
    return _Py_INIT_OK();
}


static void
preconfig_init_coerce_c_locale(_PyPreConfig *config)
{
    if (!config->configure_locale) {
        config->coerce_c_locale = 0;
        config->coerce_c_locale_warn = 0;
        return;
    }

    const char *env = _Py_GetEnv(config->use_environment, "PYTHONCOERCECLOCALE");
    if (env) {
        if (strcmp(env, "0") == 0) {
            if (config->coerce_c_locale < 0) {
                config->coerce_c_locale = 0;
            }
        }
        else if (strcmp(env, "warn") == 0) {
            if (config->coerce_c_locale_warn < 0) {
                config->coerce_c_locale_warn = 1;
            }
        }
        else {
            if (config->coerce_c_locale < 0) {
                config->coerce_c_locale = 1;
            }
        }
    }

    /* Test if coerce_c_locale equals to -1 or equals to 1:
       PYTHONCOERCECLOCALE=1 doesn't imply that the C locale is always coerced.
       It is only coerced if if the LC_CTYPE locale is "C". */
    if (config->coerce_c_locale < 0 || config->coerce_c_locale == 1) {
        /* The C locale enables the C locale coercion (PEP 538) */
        if (_Py_LegacyLocaleDetected()) {
            config->coerce_c_locale = 2;
        }
        else {
            config->coerce_c_locale = 0;
        }
    }

    if (config->coerce_c_locale_warn < 0) {
        config->coerce_c_locale_warn = 0;
    }
}


static _PyInitError
preconfig_init_allocator(_PyPreConfig *config)
{
    if (config->allocator == PYMEM_ALLOCATOR_NOT_SET) {
        /* bpo-34247. The PYTHONMALLOC environment variable has the priority
           over PYTHONDEV env var and "-X dev" command line option.
           For example, PYTHONMALLOC=malloc PYTHONDEVMODE=1 sets the memory
           allocators to "malloc" (and not to "debug"). */
        const char *envvar = _Py_GetEnv(config->use_environment, "PYTHONMALLOC");
        if (envvar) {
            PyMemAllocatorName name;
            if (_PyMem_GetAllocatorName(envvar, &name) < 0) {
                return _Py_INIT_ERR("PYTHONMALLOC: unknown allocator");
            }
            config->allocator = (int)name;
        }
    }

    if (config->dev_mode && config->allocator == PYMEM_ALLOCATOR_NOT_SET) {
        config->allocator = PYMEM_ALLOCATOR_DEBUG;
    }
    return _Py_INIT_OK();
}


static _PyInitError
preconfig_read(_PyPreConfig *config, _PyPreCmdline *cmdline)
{
    _PyInitError err;

    err = _PyPreCmdline_Read(cmdline, config);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    _PyPreCmdline_SetPreConfig(cmdline, config);

    /* legacy_windows_fs_encoding, coerce_c_locale, utf8_mode */
#ifdef MS_WINDOWS
    _Py_get_env_flag(config->use_environment,
                     &config->legacy_windows_fs_encoding,
                     "PYTHONLEGACYWINDOWSFSENCODING");
#endif

    preconfig_init_coerce_c_locale(config);

    err = preconfig_init_utf8_mode(config, cmdline);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    /* allocator */
    err = preconfig_init_allocator(config);
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    assert(config->coerce_c_locale >= 0);
    assert(config->coerce_c_locale_warn >= 0);
#ifdef MS_WINDOWS
    assert(config->legacy_windows_fs_encoding >= 0);
#endif
    assert(config->utf8_mode >= 0);
    assert(config->isolated >= 0);
    assert(config->use_environment >= 0);
    assert(config->dev_mode >= 0);

    return _Py_INIT_OK();
}


/* Read the configuration from:

   - command line arguments
   - environment variables
   - Py_xxx global configuration variables
   - the LC_CTYPE locale */
_PyInitError
_PyPreConfig_Read(_PyPreConfig *config, const _PyArgv *args)
{
    _PyInitError err;

    err = _PyRuntime_Initialize();
    if (_Py_INIT_FAILED(err)) {
        return err;
    }

    _PyPreConfig_GetGlobalConfig(config);

    /* Copy LC_CTYPE locale, since it's modified later */
    const char *loc = setlocale(LC_CTYPE, NULL);
    if (loc == NULL) {
        return _Py_INIT_ERR("failed to LC_CTYPE locale");
    }
    char *init_ctype_locale = _PyMem_RawStrdup(loc);
    if (init_ctype_locale == NULL) {
        return _Py_INIT_NO_MEMORY();
    }

    /* Save the config to be able to restore it if encodings change */
    _PyPreConfig save_config;
    _PyPreConfig_InitFromPreConfig(&save_config, config);

    /* Set LC_CTYPE to the user preferred locale */
    if (config->configure_locale) {
        _Py_SetLocaleFromEnv(LC_CTYPE);
    }

    _PyPreCmdline cmdline = _PyPreCmdline_INIT;
    int init_utf8_mode = Py_UTF8Mode;
#ifdef MS_WINDOWS
    int init_legacy_encoding = Py_LegacyWindowsFSEncodingFlag;
#endif

    if (args) {
        err = _PyPreCmdline_SetArgv(&cmdline, args);
        if (_Py_INIT_FAILED(err)) {
            goto done;
        }
    }

    int locale_coerced = 0;
    int loops = 0;

    while (1) {
        int utf8_mode = config->utf8_mode;

        /* Watchdog to prevent an infinite loop */
        loops++;
        if (loops == 3) {
            err = _Py_INIT_ERR("Encoding changed twice while "
                               "reading the configuration");
            goto done;
        }

        /* bpo-34207: Py_DecodeLocale() and Py_EncodeLocale() depend
           on Py_UTF8Mode and Py_LegacyWindowsFSEncodingFlag. */
        Py_UTF8Mode = config->utf8_mode;
#ifdef MS_WINDOWS
        Py_LegacyWindowsFSEncodingFlag = config->legacy_windows_fs_encoding;
#endif

        err = preconfig_read(config, &cmdline);
        if (_Py_INIT_FAILED(err)) {
            goto done;
        }

        /* The legacy C locale assumes ASCII as the default text encoding, which
         * causes problems not only for the CPython runtime, but also other
         * components like GNU readline.
         *
         * Accordingly, when the CLI detects it, it attempts to coerce it to a
         * more capable UTF-8 based alternative.
         *
         * See the documentation of the PYTHONCOERCECLOCALE setting for more
         * details.
         */
        int encoding_changed = 0;
        if (config->coerce_c_locale && !locale_coerced) {
            locale_coerced = 1;
            _Py_CoerceLegacyLocale(0);
            encoding_changed = 1;
        }

        if (utf8_mode == -1) {
            if (config->utf8_mode == 1) {
                /* UTF-8 Mode enabled */
                encoding_changed = 1;
            }
        }
        else {
            if (config->utf8_mode != utf8_mode) {
                encoding_changed = 1;
            }
        }

        if (!encoding_changed) {
            break;
        }

        /* Reset the configuration before reading again the configuration,
           just keep UTF-8 Mode value. */
        int new_utf8_mode = config->utf8_mode;
        int new_coerce_c_locale = config->coerce_c_locale;
        _PyPreConfig_Copy(config, &save_config);
        config->utf8_mode = new_utf8_mode;
        config->coerce_c_locale = new_coerce_c_locale;

        /* The encoding changed: read again the configuration
           with the new encoding */
    }
    err = _Py_INIT_OK();

done:
    if (init_ctype_locale != NULL) {
        setlocale(LC_CTYPE, init_ctype_locale);
        PyMem_RawFree(init_ctype_locale);
    }
    Py_UTF8Mode = init_utf8_mode ;
#ifdef MS_WINDOWS
    Py_LegacyWindowsFSEncodingFlag = init_legacy_encoding;
#endif
    _PyPreCmdline_Clear(&cmdline);
    return err;
}


/* Write the pre-configuration:

   - set the memory allocators
   - set Py_xxx global configuration variables
   - set the LC_CTYPE locale (coerce C locale, PEP 538) and set the UTF-8 mode
     (PEP 540)

   If the memory allocator is changed, config is re-allocated with new
   allocator. So calling _PyPreConfig_Clear(config) is safe after this call.

   Do nothing if called after Py_Initialize(): ignore the new
   pre-configuration. */
_PyInitError
_PyPreConfig_Write(const _PyPreConfig *config)
{
    if (_PyRuntime.core_initialized) {
        /* bpo-34008: Calling this functions after Py_Initialize() ignores
           the new configuration. */
        return _Py_INIT_OK();
    }

    PyMemAllocatorName name = (PyMemAllocatorName)config->allocator;
    if (name != PYMEM_ALLOCATOR_NOT_SET) {
        if (_PyMem_SetupAllocators(name) < 0) {
            return _Py_INIT_ERR("Unknown PYTHONMALLOC allocator");
        }
    }

    _PyPreConfig_SetGlobalConfig(config);

    if (config->configure_locale) {
        if (config->coerce_c_locale) {
            _Py_CoerceLegacyLocale(config->coerce_c_locale_warn);
        }

        /* Set LC_CTYPE to the user preferred locale */
        _Py_SetLocaleFromEnv(LC_CTYPE);
    }

    /* Write the new pre-configuration into _PyRuntime */
    _PyPreConfig_Copy(&_PyRuntime.preconfig, config);

    return _Py_INIT_OK();
}
