#include "luartos.h"

#include "lua.h"
#include "lauxlib.h"

#include <sys/dirent.h>
#include <sys/stat.h>
#include <unistd.h> 
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/syslog.h>
#include <drivers/uart.h>
#include "sys/status.h"
#include "drivers/gpio.h"
#include "ymodem.h"

#define l_getc(f)		getc(f)
#define l_lockfile(f)   ((void)0)
#define l_unlockfile(f)	((void)0)

int ioctl(int fd, unsigned long request, ...);

static int f_attributes(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char type[16];
    char ftime[24];
    lua_Integer size = 0;
    struct tm *tm_info;

    struct stat s;

    strcpy(type, "notfound");
    strcpy(ftime, "01/01/2000 00:00");

    if (stat(path, &s) >= 0) {
		if (S_ISDIR(s.st_mode)) {
			strcpy(type, "directory");
		}
		else if (S_ISREG(s.st_mode)) {
			strcpy(type, "file");
		}

		size = s.st_size;

		tm_info = localtime(&s.st_mtime);
		strftime(ftime, 24, "%d/%m/%Y %R", tm_info);
    }

    lua_pushstring(L, type);
    lua_pushinteger(L, size);
    lua_pushstring(L, ftime);

	return 2;
}

static int read_line (lua_State *L, FILE *f, int chop) {
  luaL_Buffer b;
  char c = '\0';

  luaL_buffinit(L, &b);
  
  char nl = '\n';
  
  if (f == stdin) {
      nl = '\r';
  }
  
  while ((c != (char)EOF) && (c != nl)) {  /* repeat until end of line */
    char *buff = luaL_prepbuffer(&b);  /* pre-allocate buffer */
    int i = 0;
    l_lockfile(f);  /* no memory errors can happen inside the lock */
    while ((i < LUAL_BUFFERSIZE) && ((c = l_getc(f)) != (char)EOF) && (c != nl)) {
      buff[i++] = c;
    
      if (f == stdin) {
          putc(c, stdout);
      }
    }
    
    l_unlockfile(f);
    luaL_addsize(&b, i);
  }
  if (!chop && c == nl)  /* want a newline and have one? */
    luaL_addchar(&b, c);  /* add ending newline to result */
  luaL_pushresult(&b);  /* close buffer */
  /* return ok if read something (either a newline or something else) */
  return (c == nl || lua_rawlen(L, -1) > 0);
}

static int f_receive (lua_State *L) {
    const char *filename = luaL_optstring(L, 1, "");
    unsigned int i;
    int done;

    unsigned char chunk[255];
    unsigned char *cchunk;
    unsigned char chunk_size;

    int buff_size = 10240;

    if (strlen(filename) == 0) return 0;

    FILE *f= fopen(filename, "w");
    if (f) {
    	// Try to allocate a great buffer for output stream
    	while (setvbuf(f , NULL, _IOFBF, buff_size) != 0) {
    		buff_size = buff_size - 1024;
    	}

        uart_lock(CONSOLE_UART);
        
        // Clear received buffer
        uart_consume(CONSOLE_UART);

        // Send 'C' for start
        uart_write(CONSOLE_UART, 'C');
        uart_write(CONSOLE_UART, '\n');

        done = 0;

        for(;;) {
            // Wait for chunk size
            if (!uart_read(CONSOLE_UART, (char *)&chunk_size, 2000)) {
                break;
            }

            // More chunks?
            if (chunk_size == 0) {
                done = 1;
                break;
            }

            // Read chunk
            cchunk = chunk;
            for(i=0; i < chunk_size; i++) {
                if (!uart_read(CONSOLE_UART, (char *)cchunk++, 2000)) {
                    break;
                }
            }

            // Wrhite chunk to disk
            fwrite(chunk,chunk_size,1,f);

            // Send 'C' for start
            uart_write(CONSOLE_UART, 'C');
            uart_write(CONSOLE_UART, '\n');
        }
                
        fclose(f);
        
        if (!done) {
            uart_unlock(CONSOLE_UART);
            
            return luaL_error(L, "timeout");
        }

        uart_unlock(CONSOLE_UART);
    } else {
        return luaL_error(L, strerror(errno));
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int f_send (lua_State *L) {
    const char *filename = luaL_optstring(L, 1, "");
    int i;
    int done;
    int error;
    char c;

    unsigned char chunk[255];
    unsigned char *cchunk;
    unsigned char chunk_size;
  
    if (strlen(filename) == 0) return 0;

    FILE *f= fopen(filename, "r");
    if (f) {
    	uart_lock(CONSOLE_UART);

        done = 1;
        error = 0;
        while (!feof(f)) {
            // Read next chunk
            chunk_size = 0;
            cchunk = chunk;
            for(i=0; i < 255; i++) {
                if (fread(cchunk++,1,1,f) == 1) {
                    chunk_size++;
                }
            }
            
            // Wait for C\n
            if (!uart_read(CONSOLE_UART, &c, 2000)) {done = 0; break;}
            if (c != 'C') {done = 0; error = 1; break;}
            
            if (!uart_read(CONSOLE_UART, &c, 2000)) {done = 0; break;}
            if (c != '\n') {done = 0; error = 1; break;}
            
            // Send chunk size
            uart_write(CONSOLE_UART, chunk_size);

            // Send chunk
            cchunk = chunk;
            while(chunk_size--) {
                uart_write(CONSOLE_UART, *cchunk++);
            }  
        }
                
        fclose(f);
        
        if (done){
            // Send last chunk with size 0

            // Wait for C\n
            if (!uart_read(CONSOLE_UART, &c, 2000)) {uart_unlock(CONSOLE_UART);return luaL_error(L, "timeout");}
            if (c != 'C') {uart_unlock(CONSOLE_UART);return luaL_error(L, "unnexpected input");}
            
            if (!uart_read(CONSOLE_UART, &c, 2000)) {uart_unlock(CONSOLE_UART);return luaL_error(L, "timeout");}
            if (c != '\n') {uart_unlock(CONSOLE_UART);return luaL_error(L, "unnexpected input");}
            
            // Send chunk size
            chunk_size = '\0';
            uart_write(CONSOLE_UART, chunk_size);
        } else {
            uart_unlock(CONSOLE_UART);

            if (error) {
            	return luaL_error(L, "unnexpected input");
            } else {
            	return luaL_error(L, "timeout");
            }
        }
                
        uart_unlock(CONSOLE_UART);

    } else {
        return luaL_error(L, strerror(errno));
    }

    return 0;
}

static int f_ymreceive (lua_State *L) {
    const char *filename = luaL_optstring(L, 1, "");
    int result;
    char newname[128];
    char origname[128] = {"\0"};

    if (strlen(filename) == 0) sprintf(newname,"_ym_recv.tmp");
    else sprintf(newname,"%s", filename);

    FILE *f= fopen(newname, "w");
    if (f) {
        uart_lock(CONSOLE_UART);

        // Clear received buffer
		status_set(STATUS_UART_QUE_NOTFILTER);
        uart_consume(CONSOLE_UART);

        result = Ymodem_Receive(f, YM_MAX_FILESIZE, origname, 0);

        fclose(f);
        uart_consume(CONSOLE_UART);
        uart_writes(CONSOLE_UART, "\r\n");
		status_clear(STATUS_UART_QUE_NOTFILTER);
        uart_unlock(CONSOLE_UART);

        if (result <= 0) {
            return luaL_error(L, "error receiving file (%d)", result);
        }
        if ((strlen(filename) == 0) && (strlen(origname) != 0)) {
        	if (rename(newname,origname) < 0) lua_pushboolean(L, false);
        	else lua_pushboolean(L, true);
        }
    	else lua_pushboolean(L, true);
    } else {
        return luaL_error(L, strerror(errno));
    }

	lua_pushstring(L, origname);
    return 2;
}

static int f_ymsend (lua_State *L) {
    const char *filename = luaL_optstring(L, 1, "");
    char *basename;
    lua_Integer result;
    struct stat sb;

    if (strlen(filename) == 0) return 0;

    basename = strrchr(filename, '/');
    if (basename == NULL) basename = (char *)filename;
    else basename++;
    if (strlen(basename) == 0) return 0;

    if (stat(filename, &sb) != 0) {
        return luaL_error(L, strerror(errno));
    }

    FILE *f= fopen(filename, "r");
    if (f) {
    	uart_lock(CONSOLE_UART);
		status_set(STATUS_UART_QUE_NOTFILTER);

    	result = Ymodem_Transmit(basename, sb.st_size, f);
    	if (result == 0) result = sb.st_size;
		#if USE_LED_ACT
		gpio_pin_clr(LED_ACT);
		#endif

        fclose(f);

        uart_writes(CONSOLE_UART, "\r\n");
		status_clear(STATUS_UART_QUE_NOTFILTER);
        uart_unlock(CONSOLE_UART);

    } else {
        return luaL_error(L, strerror(errno));
    }

    lua_pushinteger(L, result);
    return 1;
}

#undef l_getc
#undef l_lockfile
#undef l_unlockfile
