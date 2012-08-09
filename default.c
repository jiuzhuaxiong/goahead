/*
    default.c -- Default URL handler. Includes support for ASP.
  
    This module provides default URL handling and Active Server Page support.  
 */

/********************************* Includes ***********************************/

#include    "goahead.h"

/*********************************** Locals ***********************************/

static char_t   *websDefaultPage;           /* Default page name */
static char_t   *websDefaultDir;            /* Default Web page directory */

/**************************** Forward Declarations ****************************/

#define MAX_URL_DEPTH           8   /* Max directory depth of websDefaultDir */

static void websDefaultWriteEvent(webs_t wp);

/*********************************** Code *************************************/
/*
    Process a default URL request. This will validate the URL and handle "../" and will provide support for Active
    Server Pages. As the handler is the last handler to run, it always indicates that it has handled the URL by
    returning 1. 
 */
int websDefaultHandler(webs_t wp, char_t *urlPrefix, char_t *webDir, int arg, char_t *url, char_t *path, char_t *query)
{
    websStatType    sbuf;
    char_t          *lpath, *tmp, *date;
    ssize           nchars;
    int             code;

    a_assert(websValid(wp));
    a_assert(url && *url);
    a_assert(path);
    a_assert(query);

    /*
        Validate the URL and ensure that ".."s don't give access to unwanted files
     */
    if (websValidateUrl(wp, path) < 0) {
        /* 
            preventing a cross-site scripting exploit -- you may restore the following line of code to revert to the
            original behavior...  websError(wp, 500, T("Invalid URL %s"), url);
        */
        websError(wp, 500, T("Invalid URL"));
        return 1;
    }
    lpath = websGetRequestLpath(wp);
    nchars = gstrlen(lpath) - 1;
    if (lpath[nchars] == '/' || lpath[nchars] == '\\') {
        lpath[nchars] = '\0';
    }

    /*
        If the file is a directory, redirect using the nominated default page
     */
    if (websPageIsDirectory(lpath)) {
        nchars = gstrlen(path);
        if (path[nchars-1] == '/' || path[nchars-1] == '\\') {
            path[--nchars] = '\0';
        }
        nchars += gstrlen(websDefaultPage) + 2;
        fmtAlloc(&tmp, nchars, T("%s/%s"), path, websDefaultPage);
        websRedirect(wp, tmp);
        bfree(tmp);
        return 1;
    }
    /*
        Open the document. Stat for later use.
     */
    if (websPageOpen(wp, lpath, path, O_RDONLY | O_BINARY, 0666) < 0) {
        websError(wp, 404, T("Cannot open URL"));
        return 1;
    } 
    if (websPageStat(wp, lpath, path, &sbuf) < 0) {
        websError(wp, 400, T("Cannot stat page for URL"));
        return 1;
    }

    /*
        If the page has not been modified since the user last received it and it is not dynamically generated each time
        (ASP), then optimize request by sending a 304 Use local copy response.
     */
    websStats.localHits++;
    code = 200;
#if BIT_IF_MODIFIED
    if (wp->flags & WEBS_IF_MODIFIED && !(wp->flags & WEBS_ASP)) {
        if (sbuf.mtime <= wp->since) {
            code = 304;
#if UNUSED
            websWriteHeader(wp, T("HTTP/1.0 304 Use local copy\r\n"));
            /*
                NOTE: by license terms the following line of code must not be modified.
             */
            websWriteHeader(wp, T("Server: GoAhead/%s\r\n"), BIT_VERSION);
            if (wp->flags & WEBS_KEEP_ALIVE) {
                websWriteHeader(wp, T("Connection: keep-alive\r\n"));
            } else {
                websWriteHeader(wp, T("Connection: close\r\n"));
            }
            websWriteHeader(wp, T("\r\n"));
            wp->flags |= WEBS_HEADER_DONE;
            websDone(wp, 304);
            return 1;
#endif
        }
    }
#endif

#if UNUSED
    /*
        Output the normal HTTP response header
        MOB OPT - compute date periodically
     */
    if ((date = websGetDateString(NULL)) != NULL) {
        websWriteHeader(wp, T("HTTP/1.0 200 OK\r\nDate: %s\r\n"), date);
        /*
            The Server HTTP header below must not be modified unless explicitly allowed by licensing terms.
         */
        websWriteHeader(wp, T("Server: GoAhead/%s\r\n"), BIT_VERSION);
        bfree(date);
    }
    wp->flags |= WEBS_HEADER_DONE;

    /*
        If this is an ASP request, ensure the remote browser doesn't cache it.
        Send back both HTTP/1.0 and HTTP/1.1 cache control directives
     */
    if (wp->flags & WEBS_ASP) {
        bytes = 0;
        websWriteHeader(wp, T("Pragma: no-cache\r\nCache-Control: no-cache\r\n"));

    } else {
        if ((date = websGetDateString(&sbuf)) != NULL) {
            websWriteHeader(wp, T("Last-modified: %s\r\n"), date);
            bfree(date);
        }
        bytes = sbuf.size;
    }
    //  MOB - transfer chunking
    if (wp->flags & WEBS_HEAD_REQUEST) {
        websWriteHeader(wp, T("Content-length: %d\r\n"), bytes);
    } else if (bytes) {
        websWriteHeader(wp, T("Content-length: %d\r\n"), bytes);
        websSetRequestBytes(wp, bytes);
    }
    websWriteHeader(wp, T("Content-type: %s\r\n"), websGetRequestType(wp));

    if (wp->flags & WEBS_KEEP_ALIVE) {
        websWriteHeader(wp, T("Connection: keep-alive\r\n"));
    } else {
        websWriteHeader(wp, T("Connection: close\r\n"));
    }
    websWriteHeader(wp, T("\r\n"));
#else
    websWriteHeaders(wp, code, (wp->flags & WEBS_ASP) ? -1 : sbuf.size, 0);
    if (!(wp->flags & WEBS_ASP)) {
        if ((date = websGetDateString(&sbuf)) != NULL) {
            websWriteHeader(wp, T("Last-modified: %s\r\n"), date);
            bfree(date);
        }
    }
    websWriteHeader(wp, T("\r\n"));
#endif

    /*
        All done if the browser did a HEAD request
     */
    if (wp->flags & WEBS_HEAD_REQUEST) {
        websDone(wp, 200);
        return 1;
    }
#if BIT_JAVASCRIPT
    /*
        Evaluate ASP requests
     */
    if (wp->flags & WEBS_ASP) {
        if (websAspRequest(wp, lpath) < 0) {
            return 1;
        }
        websDone(wp, 200);
        return 1;
    }
#endif
    /*
        Return the data via background write
     */
    websSetRequestSocketHandler(wp, SOCKET_WRITABLE, websDefaultWriteEvent);
    return 1;
}


#if WINDOWS
static int badPath(char_t* path, char_t* badPath, int badLen)
{
   int retval = 0;
   int len = gstrlen(path);
   int i = 0;

   if (len <= badLen + 1) {
      for (i = 0; i < badLen; ++i) {
         if (badPath[i] != gtolower(path[i])) {
            return 0;
         }
      }
      /* 
            If we get here, the first 'badLen' characters match.  If 'path' is 1 character larger than 'badPath' and
            that extra character is NOT a letter or a number, we have a bad path.
       */
      retval = 1;
      if (badLen + 1 == len) {
         /* e.g. path == "aux:" */
         if (gisalnum(path[len-1])) {
            /* 
                the last character is alphanumeric, so we let this path go through. 
             */
            retval = 0;
         }
      }
   }
   return retval;
}


/*
    If we're running on Windows 95/98/ME, malicious users can crash the OS by requesting an URL with any of several
    reserved DOS device names in them (AUX, NUL, etc.).  If we're running on any of those OS versions, we scan the
    URL for paths with any of these elements before trying to access them. If any of the subdirectory names match
    one of our prohibited links, we declare this to be a 'bad' path, and return 1 to indicate this. This may be a
    heavy-handed approach, but should prevent the DOS attack.  NOTE that this function is only compiled in when we
    are running on Win32, and only has an effect when running on Win95/98, or ME. On all other versions of Windows,
    we check the version info, and return 0 immediately.
 
    According to http://packetstormsecurity.nl/0003-exploits/SCX-SA-01.txt: II.  Problem Description When the
    Microsoft Windows operating system is parsing a path that is being crafted like "c:\[device]\[device]" it will
    halt, and crash the entire operating system.  Four device drivers have been found to crash the system.  The CON,
    NUL, AUX, CLOCK$ and CONFIG$ are the two device drivers which are known to crash.  Other devices as LPT[x]:,
    COM[x]: and PRN have not been found to crash the system.  Making combinations as CON\NUL, NUL\CON, AUX\NUL, ...
    seems to crash Ms Windows as well.  Calling a path such as "C:\CON\[filename]" won't result in a crash but in an
    error-message.  Creating the map "CON", "CLOCK$", "AUX" "NUL" or "CONFIG$" will also result in a simple
    error-message saying: ''creating that map isn't allowed''.
 
    returns 1 if it finds a bad path element.
*/
static int isBadWindowsPath(char_t** parts, int partCount)
{
    OSVERSIONINFO version;
    int i;
    version.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    if (GetVersionEx(&version)) {
        if (VER_PLATFORM_WIN32_NT != version.dwPlatformId) {
            /*
                we are currently running on 95/98/ME.
             */
            for (i = 0; i < partCount; ++i) {
                /*
                   check against the prohibited names. If any of our requested 
                   subdirectories match any of these, return '1' immediately.
                 */
                if ( 
                    (badPath(parts[i], T("con"), 3)) ||
                    (badPath(parts[i], T("com"), 3)) ||
                    (badPath(parts[i], T("nul"), 3)) ||
                    (badPath(parts[i], T("aux"), 3)) ||
                    (badPath(parts[i], T("clock$"), 6)) ||
                    (badPath(parts[i], T("config$"), 7)) ) {
                    return 1;
                }
            }
        }
    }
    /*
        Either we're not on one of the bad OS versions, or the request has no problems.
     */
    return 0;
}
#endif


/*
    Validate the URL path and process ".." path segments. Return -1 if the URL is bad.
 */
int websValidateUrl(webs_t wp, char_t *path)
{
    char_t  *parts[MAX_URL_DEPTH];  /* Array of ptr's to URL parts */
    char_t  *token, *dir, *lpath; 
    int       i, len, npart;

    a_assert(websValid(wp));
    a_assert(path);

    dir = websGetRequestDir(wp);
    if (dir == NULL || *dir == '\0') {
        return -1;
    }
    /*
        Copy the string so we don't destroy the original
     */
    path = bstrdup(path);
    websDecodeUrl(path, path, gstrlen(path));
    len = npart = 0;
    parts[0] = NULL;
    token = gstrchr(path, '\\');
    while (token != NULL) {
        *token = '/';
        token = gstrchr(token, '\\');
    }
    token = gstrtok(path, T("/"));

    /*
        Look at each directory segment and process "." and ".." segments. Don't allow the browser to pop outside the
        root web
    */
    while (token != NULL) {
        if (npart >= MAX_URL_DEPTH) {
             /*
              * malformed URL -- too many parts for us to process.
              */
             bfree(path);
             return -1;
        }
        if (gstrcmp(token, T("..")) == 0) {
            if (npart > 0) {
                npart--;
            }
        } else if (gstrcmp(token, T(".")) != 0) {
            parts[npart] = token;
            len += gstrlen(token) + 1;
            npart++;
        }
        token = gstrtok(NULL, T("/"));
    }

#if WINDOWS
   if (isBadWindowsPath(parts, npart)) {
      bfree(path);
      return -1;
   }
#endif

    /*
        Create local path for document. Need extra space all "/" and null.
     */
    if (npart || (gstrcmp(path, T("/")) == 0) || (path[0] == '\0')) {
        lpath = balloc((gstrlen(dir) + 1 + len + 1) * sizeof(char_t));
        gstrcpy(lpath, dir);
        for (i = 0; i < npart; i++) {
            gstrcat(lpath, T("/"));
            gstrcat(lpath, parts[i]);
        }
        websSetRequestLpath(wp, lpath);
        bfree(path);
        bfree(lpath);
    } else {
        bfree(path);
        return -1;
    }
    return 0;
}


/*
    Do output back to the browser in the background. This is a socket write handler.
 */
static void websDefaultWriteEvent(webs_t wp)
{
    char    *buf;
    ssize   len, wrote, written, bytes;
    int     flags;

    a_assert(websValid(wp));

    flags = websGetRequestFlags(wp);
    websSetTimeMark(wp);
    wrote = bytes = 0;
    written = websGetRequestWritten(wp);

    /*
        We only do this for non-ASP documents
     */
    if (!(flags & WEBS_ASP)) {
        bytes = websGetRequestBytes(wp);
        /*
            Note: websWriteDataNonBlock may return less than we wanted. It will return -1 on a socket error
         */
        if ((buf = balloc(PAGE_READ_BUFSIZE)) == NULL) {
            websError(wp, 200, T("Can't get memory"));
        } else {
            while ((len = websPageReadData(wp, buf, PAGE_READ_BUFSIZE)) > 0) {
                if ((wrote = websWriteDataNonBlock(wp, buf, len)) < 0) {
                    break;
                }
                written += wrote;
                if (wrote != len) {
                    websPageSeek(wp, - (len - wrote));
                    break;
                }
            }
            /*
                Safety: If we are at EOF, we must be done
             */
            if (len == 0) {
                a_assert(written >= bytes);
                written = bytes;
            }
            bfree(buf);
        }
    }
    /*
        We're done if an error, or all bytes output
     */
    websSetRequestWritten(wp, written);
    if (wrote < 0 || written >= bytes) {
        websDone(wp, 200);
    }
}


/* 
    Initialize variables and data for the default URL handler module
 */

void websDefaultOpen()
{
    websDefaultPage = bstrdup(T("index.html"));
}


/* 
    Closing down. Free resources.
 */
void websDefaultClose()
{
    bfree(websDefaultPage);
    websDefaultPage = NULL;
    bfree(websDefaultDir);
    websDefaultDir = NULL;
}


/*
    Get the default page for URL requests ending in "/"
 */
char_t *websGetDefaultPage()
{
    return websDefaultPage;
}


char_t *websGetDefaultDir()
{
    return websDefaultDir;
}


/*
    Set the default page for URL requests ending in "/"
 */
void websSetDefaultPage(char_t *page)
{
    a_assert(page && *page);

    if (websDefaultPage) {
        bfree(websDefaultPage);
    }
    websDefaultPage = bstrdup(page);
}


/*
    Set the default web directory
 */
void websSetDefaultDir(char_t *dir)
{
    a_assert(dir && *dir);
    if (websDefaultDir) {
        bfree(websDefaultDir);
    }
    websDefaultDir = bstrdup(dir);
}


int websDefaultHomePageHandler(webs_t wp, char_t *urlPrefix, char_t *webDir, int arg, char_t *url, 
        char_t *path, char_t *query)
{
    //  MOB gmatch
    //  MOB - could groutines apply T() to args)
    if (*url == '\0' || gstrcmp(url, T("/")) == 0) {
        websRedirect(wp, "index.html");
        return 1;
    }
    return 0;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis GoAhead open source license or you may acquire 
    a commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
