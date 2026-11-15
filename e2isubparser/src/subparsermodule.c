#include <Python.h>

#include "vlc/include/subtitles.h"
#include "ffmpeg/include/htmlsubtitles.h"
#include "html/include/htmlcleaner.h"

#define IPTV_ULL_TYPE unsigned long long 
#define IPTV_LL_TYPE long long 
#define IPTV_UI_TYPE unsigned int 

static const char SUB_PARSER_VERSION[] = "0.9.1";
static const IPTV_UI_TYPE MAX_SUBTITLE_TEXT_SIZE = 4096;


static PyObject * get_version(PyObject *self, PyObject *unused)
{
#if PY_MAJOR_VERSION >= 3
    return PyUnicode_FromString(SUB_PARSER_VERSION);
#else
    return PyString_FromString(SUB_PARSER_VERSION);
#endif
}

static uint32_t words(const char sentence[ ])
{
    uint32_t counted = 0;
    const char *it = sentence;
    uint32_t inword = 0;

    do
    {
        switch(*it) 
        {
            case '\0': 
            case ' ': 
            case '\t': 
            case '\n': 
            case '\r':
                if (inword) 
                {
                    inword = 0; 
                    counted++;
                }
                break;
            default: inword = 1;
        } 
    }while(*it++);

    return counted;
}

/* PUT_CH in ff_htmlmarkup_to_ass() cuts at a byte limit: drop an incomplete
 * UTF-8 sequence at the end, Py_BuildValue("s") would fail on it */
static void utf8_trim_incomplete(char *str)
{
    size_t len = strlen(str);
    size_t i = len;
    size_t need = 0;
    unsigned char c = 0;
    /* go back to the lead byte of the last character (max. 3 continuation bytes) */
    while (i > 0 && len - i < 4 && (((unsigned char)str[i - 1]) & 0xC0) == 0x80)
    {
        --i;
    }
    if (0 == i)
    {
        return;
    }
    c = (unsigned char)str[i - 1];
    if (c < 0x80)
    {
        return;
    }
    need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    if ((len - (i - 1)) < need)
    {
        str[i - 1] = '\0';
    }
}

static char * ass_get_text(char *str)
{
    // Events are stored in the Block in this order:
    // ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text
    // 91,0,Default,,0,0,0,,maar hij smaakt vast tof.
    int i = 0;
    char *p_str = str;
    while(i < 8 && *p_str != '\0')
    {
        if (*p_str == ',')
            i++;
        p_str++;
    }
    // replace new line tat to real new line: '\\N' and '\\n' -> ' \n
    char *p_newline = NULL;
    while(((p_newline = strstr(p_str, "\\N")) != NULL) || ((p_newline = strstr(p_str, "\\n")) != NULL))
    {
        *(p_newline) = ' ';
        *(p_newline + 1) = '\n';
    }
    return p_str;
}

static char *get_text(char *str, const int i_type, int b_removeTags, char *tmpBuffer, size_t tmpBufferSize)
{
    char *strPtr = str;
    if (0 != b_removeTags)
    {
        if (SUB_TYPE_SSA1 == i_type ||
            SUB_TYPE_SSA2_4 == i_type ||
            SUB_TYPE_ASS == i_type)
        {
            strPtr = ass_get_text(str);
        }

        ff_htmlmarkup_to_ass(NULL, tmpBuffer, tmpBufferSize, strPtr);
        utf8_trim_incomplete(tmpBuffer);
        return tmpBuffer;
    }
    return str;
}

static PyObject * _subparser_parse(PyObject *self, PyObject *args)
{
    const char *inputStr = NULL;
    int i_microsecperframe = 0;
    int b_setEndTime = 1;
    int b_removeTags = 1;
    int i_CPS = 12;
    int i_WPM = 138;
    int i = 0;
    char *pszText = NULL;
    PyObject *retObj = NULL; 
    PyObject *list = NULL;
    PyObject *elem = NULL;
    demux_sys_t *p_sys = NULL;
    
    if (!PyArg_ParseTuple(args, "si|iiii", &inputStr, &i_microsecperframe, &b_removeTags, &b_setEndTime, &i_CPS, &i_WPM))
    {
        return NULL;
    }
    
    if (0 == i_CPS) 
    {
        PyErr_SetString(PyExc_ZeroDivisionError, "CPS can not be set to 0");
        return NULL;
    }
    
    if (0 == i_WPM) 
    {
        PyErr_SetString(PyExc_ZeroDivisionError, "WPM can not be set to 0");
        return NULL;
    }
    
    pszText = malloc(MAX_SUBTITLE_TEXT_SIZE);
    if (NULL == pszText)
    {
        PyErr_SetString(PyExc_MemoryError, "Can not allocate memory for tmp buffer");
        return NULL;
    }
    
    retObj = PyDict_New();
    if (NULL == retObj)
    {
        free(pszText);
        return NULL;
    }

    if ( 0 != VLC_SubtitleDemuxOpen( inputStr, i_microsecperframe, &p_sys ) || NULL == p_sys )
    {
        free(pszText);
        return retObj;
    }
    
    if (0 != b_setEndTime)
    {
        for (i=0; i<p_sys->i_subtitles; ++i)
        {
            /* Fix previous item's end time if needed */
            if (i > 0 && 0 > p_sys->subtitle[i-1].i_stop)
            {
                /* Calculate end time based on "Subtitle reading speeds in different languages:" -> 
                 * http://www.raco.cat/index.php/QuadernsTraduccio/article/viewFile/265461/353045
                 */
                /* calc end time based on CPS (characters per second) */
                const int64_t timeBasedOnCPS = ((int64_t)strlen(p_sys->subtitle[i-1].psz_text) * 1000000) / i_CPS;
                /* calc end time based on WPM (Words per minute) */
                const int64_t timeBasedOnWPM = ((int64_t)words(p_sys->subtitle[i-1].psz_text) * 60000000) / i_WPM;
                int64_t time = timeBasedOnCPS > timeBasedOnWPM ? timeBasedOnCPS : timeBasedOnWPM;
                if (time < 1500000) /* at least 1,5s */
                {
                    time = 1500000;
                }
                
                if ( (p_sys->subtitle[i-1].i_start + time) < p_sys->subtitle[i].i_start)
                {
                    p_sys->subtitle[i-1].i_stop = p_sys->subtitle[i-1].i_start + time;
                }
                else
                {
                    p_sys->subtitle[i-1].i_stop = p_sys->subtitle[i].i_start - 10000; /* end 10ms before next subtitles */
                }
            }
            
            /* Fix last subtitle */
            if (i == (p_sys->i_subtitles - 1) && 0 > p_sys->subtitle[i].i_stop)
            {
                const int64_t timeBasedOnCPS = ((int64_t)strlen(p_sys->subtitle[i].psz_text) * 1000000) / i_CPS;
                const int64_t timeBasedOnWPM = ((int64_t)words(p_sys->subtitle[i].psz_text) * 60000000) / i_WPM;
                int64_t time = timeBasedOnCPS > timeBasedOnWPM ? timeBasedOnCPS : timeBasedOnWPM;
                if (time < 2000000) /* at least 2s */
                {
                    time = 2000000;
                }
                p_sys->subtitle[i].i_stop = p_sys->subtitle[i].i_start + time;
            }
        }
    }
    
    
    /* create list */
    list = PyList_New(p_sys->i_subtitles);
    if (NULL == list)
    {
        free(pszText);
        VLC_SubtitleDemuxClose( p_sys );
        Py_DECREF(retObj);
        return NULL;
    }

    for (i=0; i<p_sys->i_subtitles; ++i)
    {

        /* add elems to list */
        elem = Py_BuildValue("{s:I,s:I,s:s}", "start", (IPTV_UI_TYPE)(p_sys->subtitle[i].i_start / 1000), "end", (IPTV_UI_TYPE)(p_sys->subtitle[i].i_stop / 1000), "text", get_text(p_sys->subtitle[i].psz_text, p_sys->i_type, b_removeTags, pszText, MAX_SUBTITLE_TEXT_SIZE));
        if (NULL == elem)
        {
            /* e.g. text that is no valid UTF-8 - report the error, never put NULL into the list */
            free(pszText);
            VLC_SubtitleDemuxClose( p_sys );
            Py_DECREF(list);
            Py_DECREF(retObj);
            return NULL;
        }
        PyList_SetItem(list, i, elem); // still reference no need to Py_DECREF
    }
    free(pszText);
    pszText = NULL;
    
    /* add created list to return dict */
    PyDict_SetItemString(retObj, "list", list);
    Py_DECREF(list); // PyDict_SetItemString not still reference but incement it
    
    /* add subtitles format to return dict */
    /* elem = PyInt_FromLong((long) p_sys->i_type); */
#if PY_MAJOR_VERSION >= 3
    elem = PyUnicode_FromString( p_sys->psz_type_name ? p_sys->psz_type_name : "" );
#else
    elem = PyString_FromString( p_sys->psz_type_name ? p_sys->psz_type_name : "" );
#endif

    if (elem)
    {
        PyDict_SetItemString(retObj, "type", elem);
        Py_DECREF(elem); // PyDict_SetItemString not still reference but incement it
    }
    
    VLC_SubtitleDemuxClose( p_sys );
    
    return retObj;
}

static PyObject* _subparser_strip_html_tags(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"html", NULL};
    PyObject *object = NULL;
    PyObject *string = NULL;
    char *in = NULL;
    Py_ssize_t isize =  0;
    int osize =  0;
    char *out = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O:strip_html_tags", kwlist, &string))
        return NULL;
    
#if PY_MAJOR_VERSION >= 3

    if (!PyUnicode_Check(string))
    {
        PyErr_SetString(PyExc_TypeError, "strip_html_tags: str expected");
        return NULL;
    }

    in = (char *)PyUnicode_AsUTF8AndSize(string, &isize);
    if (in == NULL) {
        return NULL; // not a string object or it contains null bytes
    }

#else

    if (!PyString_Check(string))
    {
        PyErr_SetString(PyExc_TypeError, "strip_html_tags: str expected");
        return NULL;
    }

    if (PyString_AsStringAndSize(string, &in, &isize) == -1) {
        return NULL; // not a string object or it contains null bytes
    }

#endif

    Py_BEGIN_ALLOW_THREADS
    out = strip_html_tags(in, isize, &osize);
    Py_END_ALLOW_THREADS

    if (out)
    {
#if PY_MAJOR_VERSION >= 3
        object = PyUnicode_FromStringAndSize(out, osize);
#else
        object = PyString_FromStringAndSize(out, osize);
#endif
        free(out);
    }
    else
    {
        object = string;
        Py_INCREF(object);
    }

    return object;
}


static PyMethodDef _subparserMethods[] = {
    {"strip_html_tags", (PyCFunction)_subparser_strip_html_tags,  METH_VARARGS|METH_KEYWORDS,
    PyDoc_STR("strip html tags")
    },
    {"parse", (PyCFunction)_subparser_parse, METH_VARARGS,
     "return subtitles atom list parsed from given string (inputStr, i_microsecperframe, b_removeTags, b_setEndTime, i_CPS, i_WPM\n\n"
     " inputStr - (string encoded with UTF-8)\n"
     " i_microsecperframe - (microseconds per frame - used when subtitles use fps)\n"
     " b_removeTags - optional, default: True (True, False)\n"
     " b_setEndTime - optional, default: True (True - default, False)\n"
     " i_CPS - optional, default: 12 (characters per second - used for end time calculation if needed)\n"
     " i_WPM - optional, default: 138 (words per minute - used for end time calculation if needed)\n"
    },
    {"version", (PyCFunction)get_version, METH_NOARGS, 
     "return version (str) of the sub parser module\n\n" 
    },
    {NULL,NULL,0,NULL}
};

#if PY_MAJOR_VERSION >= 3

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"_subparser",         /* m_name */
	"_subparser",        /* m_doc */
	-1,                  /* m_size */
	_subparserMethods,   /* m_methods */
	NULL,                /* m_reload */
	NULL,                /* m_traverse */
	NULL,                /* m_clear */
	NULL,                /* m_free */
};


PyMODINIT_FUNC PyInit__subparser(void) {
    Py_Initialize();
    return PyModule_Create(&moduledef);
}

#else

PyMODINIT_FUNC init_subparser(void)
{
    Py_InitModule("_subparser", _subparserMethods);
}

#endif
