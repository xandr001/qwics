/*******************************************************************************************/
/*   QWICS Server COBOL load module executor                                               */
/*                                                                                         */
/*   Author: Philipp Brune               Date: 13.09.2020                                  */
/*                                                                                         */
/*   Copyright (C) 2018 - 2020 by Philipp Brune  Email: Philipp.Brune@qwics.org            */
/*                                                                                         */
/*   This file is part of of the QWICS Server project.                                     */
/*                                                                                         */
/*   QWICS Server is free software: you can redistribute it and/or modify it under the     */
/*   terms of the GNU General Public License as published by the Free Software Foundation, */
/*   either version 3 of the License, or (at your option) any later version.               */
/*   It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;       */
/*   without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR      */
/*   PURPOSE.  See the GNU General Public License for more details.                        */
/*                                                                                         */
/*   You should have received a copy of the GNU General Public License                     */
/*   along with this project. If not, see <http://www.gnu.org/licenses/>.                  */
/*******************************************************************************************/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dlfcn.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include <libcob.h>
#include <setjmp.h>
#include "config.h"
#include "db/conpool.h"
#include "env/envconf.h"
#include "msg/queueman.h"
#include "shm/shmtpm.h"
#include "enqdeq/enqdeq.h"

#ifdef __APPLE__
#include "macosx/fmemopen.h"
#endif

#define CMDBUF_SIZE 32768

#define execSql(sql, fd) _execSql(sql, fd, 1, 0)

// Keys for thread specific data
pthread_key_t connKey;
pthread_key_t childfdKey;
pthread_key_t cmdbufKey;
pthread_key_t cmdStateKey;
pthread_key_t runStateKey;
pthread_key_t cobFieldKey;
pthread_key_t xctlStateKey;
pthread_key_t retrieveStateKey;
pthread_key_t xctlParamsKey;
pthread_key_t eibbufKey;
pthread_key_t linkAreaKey;
pthread_key_t linkAreaPtrKey;
pthread_key_t linkAreaAdrKey;
pthread_key_t commAreaKey;
pthread_key_t commAreaPtrKey;
pthread_key_t areaModeKey;
pthread_key_t linkStackKey;
pthread_key_t linkStackPtrKey;
pthread_key_t memParamsStateKey;
pthread_key_t memParamsKey;
pthread_key_t twaKey;
pthread_key_t tuaKey;
pthread_key_t allocMemKey;
pthread_key_t allocMemPtrKey;
pthread_key_t respFieldsKey;
pthread_key_t respFieldsStateKey;
pthread_key_t taskLocksKey;
pthread_key_t callStackKey;
pthread_key_t callStackPtrKey;
pthread_key_t chnBufListKey;
pthread_key_t chnBufListPtrKey;

// Callback function declared in libcob
extern int (*performEXEC)(char*, void*);
extern void* (*resolveCALL)(char*);

// SQLCA
cob_field *sqlcode = NULL;
char currentMap[9];

// Making COBOl thread safe
int runningModuleCnt = 0;
char runningModules[500][9];
pthread_mutex_t moduleMutex;
pthread_cond_t  waitForModuleChange;

pthread_mutex_t sharedMemMutex;

int mem_pool_size = -1;
#define MEM_POOL_SIZE GETENV_NUMBER(mem_pool_size,"QWICS_MEM_POOL_SIZE",100)

char *jsDir = NULL;
char *loadmodDir = NULL;
char *connectStr = NULL;

void **sharedAllocMem;
int *sharedAllocMemLen;
int *sharedAllocMemPtr = NULL;

char paramsBuf[10][256];
void *paramList[10];

unsigned char *cwa;
jmp_buf taskState;

jmp_buf *condHandler[100];

cob_module thisModule;

struct chnBuf {
    unsigned char *buf;
};

char *cobDateFormat = "YYYY-MM-dd-hh.mm.ss.uuuuu";
char *dbDateFormat = "dd-MM-YYYY hh:mm:ss.uuu";
char result[30];


unsigned char *getNextChnBuf(int size) {
    int *chnBufListPtr = (int*)pthread_getspecific(chnBufListPtrKey);
    struct chnBuf *chnBufList = (struct chnBuf *)pthread_getspecific(chnBufListKey);

    if (*chnBufListPtr < 256) {
        chnBufList[*chnBufListPtr].buf = malloc(size);
        (*chnBufListPtr)++;
        return chnBufList[(*chnBufListPtr)-1].buf;
    }
    return NULL;
}


void clearChnBufList() {
    int *chnBufListPtr = (int*)pthread_getspecific(chnBufListPtrKey);
    struct chnBuf *chnBufList = (struct chnBuf *)pthread_getspecific(chnBufListKey);
    int i;

    for (i = 0; i < (*chnBufListPtr); i++) {
        free(chnBufList[i].buf);
    }
}


void cm(int res) {
    if (res != 0) {
      fprintf(stderr,"%s%d\n","Mutex operation failed: ",res);
    }
}


int getCobType(cob_field *f) {
    if (f->attr->type == COB_TYPE_NUMERIC_BINARY) {
#ifndef WORDS_BIGENDIAN
        if (COB_FIELD_BINARY_SWAP(f))
            return COB_TYPE_NUMERIC_BINARY;
        return COB_TYPE_NUMERIC_COMP5;
#else
        return COB_TYPE_NUMERIC_BINARY;
#endif
        }
        return (int)f->attr->type;
}


// Adjust pading and scale for COBOL numeric data
char* convertNumeric(char *val, int digits, int scale, char *buf) {
    char *sep = strchr(val,'.');
    char *pos = sep;
    if (sep == NULL) {
      pos = val + strlen(val) - 1;
    }
    pos++;
    int i = 0;
    while (((*pos) != 0x00) && (i < scale)) {
      buf[digits-scale+i] = *pos;
      i++;
      pos++;
    }
    // Pad to the right with 0
    while (i < scale) {
      buf[digits-scale+i] = '0';
      i++;
    }

    pos = sep;
    if (sep == NULL) {
      pos = val + strlen(val);
    }
    pos--;
    i = digits-scale-1;
    while ((pos >= val) && (i >= 0)) {
      buf[i] = *pos;
      i--;
      pos--;
    }
    // Pad to the left with 0
    while (i >= 0) {
      buf[i] = '0';
      i--;
    }
    buf[digits] = 0x00;
    return buf;
}


void setNumericValue(long v, cob_field *cobvar) {
    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_NUMERIC) {
        char hbuf[256],buf[256];
        sprintf(buf,"%ld",v);
        cob_put_picx(cobvar->data,cobvar->size,
                    convertNumeric(buf,cobvar->attr->digits,
                                       cobvar->attr->scale,hbuf));
    }
    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_NUMERIC_PACKED) {
        cob_put_s64_comp3(v,cobvar->data,cobvar->size);
    }
    if (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) {
        cob_put_u64_compx(v,cobvar->data,cobvar->size);
    }
    if (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) {
        cob_put_s64_comp5(v,cobvar->data,cobvar->size);
    }
}


char* adjustDateFormatToDb(char *str, int len) {
    int i = 0, l = strlen(cobDateFormat), pos = 0;
    char lastc = ' ';
    if (len < l) {
        return str;
    }
    // Check if str is date
    for (i = 0; i < l; i++) {
        if ((cobDateFormat[i] == '-') || (cobDateFormat[i] == ' ') || 
            (cobDateFormat[i] == ':') || (cobDateFormat[i] == '.')) {
            if (cobDateFormat[i] != str[i]) {
                if ((i == 10) &&
                    (cobDateFormat[i] == '-') && (str[i] == ' ')) {
                   continue;
                }
                if ((i == 13) &&
                    (cobDateFormat[i] == '.') && (str[i] == ':')) {
                   continue;
                }
                if ((i == 16) &&
                    (cobDateFormat[i] == '.') && (str[i] == ':')) {
                   continue;
                }
                return str;
            }       
        }
    }

    memset(result,' ',len);
    result[len] = 0x00;

    for (i = 0; i < strlen(dbDateFormat); i++) {
        if ((dbDateFormat[i] == '-') || (dbDateFormat[i] == ' ') || 
            (dbDateFormat[i] == ':') || (dbDateFormat[i] == '.')) {
            result[i] = dbDateFormat[i];
            continue;
        } else {
            if (lastc != dbDateFormat[i]) {
                int j = 0;
                while (j < l) {
                    if (dbDateFormat[i] == cobDateFormat[j]) {
                        break;
                    }
                    j++;
                }                
                if (j < l) {
                    pos = j;
                } else {
                    return result;                    
                }
                lastc = dbDateFormat[i];
            }

            result[i] = str[pos];
            pos++;
        }
    }

    return result;
}


// Synchronizing COBOL module execution
void startModule(char *progname) {
  int found = 0;
  cm(pthread_mutex_lock(&moduleMutex));
  do {
    found = 0;
    for (int i = 0; i < runningModuleCnt; i++) {
      if (strcmp(runningModules[i],progname) == 0) {
        found = 1;
        break;
      }
    }
    if (found == 1) {
        pthread_cond_wait(&waitForModuleChange,&moduleMutex);
    }
  } while (found == 1);

  if (runningModuleCnt < 500) {
      sprintf(runningModules[runningModuleCnt],"%s",progname);
      runningModuleCnt++;
  }
  cm(pthread_mutex_unlock(&moduleMutex));
}


void endModule(char *progname) {
  int found = 0;
  cm(pthread_mutex_lock(&moduleMutex));
  for (int i = 0; i < runningModuleCnt; i++) {
    if (found == 1) {
      sprintf(runningModules[i-1],"%s",runningModules[i]);
    }
    if ((found == 0) && (strcmp(runningModules[i],progname) == 0)) {
      found = 1;
    }
  }
  if (runningModuleCnt > 0) {
    runningModuleCnt--;
  }
  cm(pthread_mutex_unlock(&moduleMutex));
  pthread_cond_broadcast(&waitForModuleChange);
}


void writeJson(char *map, char *mapset, int childfd) {
    int n = 0, l = strlen(map), found = 0, brackets = 0;
    write(childfd,"JSON=",5);
    char jsonFile[255];
    sprintf(jsonFile,"%s%s%s%s",GETENV_STRING(jsDir,"QWICS_JSDIR","../copybooks"),"/",mapset,".js");
    FILE *js = fopen(jsonFile,"rb");
    if (js != NULL) {
        while (1) {
            char c = fgetc(js);
            if (feof(js)) {
                break;
            }
            if (found == 0) {
                if (map[n] == c) {
                    n++;
                } else {
                    n = 0;
                }
                if (n == l) {
                    found = 1;
                }
            }
            if (found == 1) {
                if (c == '{') {
                    found = 2;
                }
            }
            if (found == 2) {
                write(childfd,&c,1);
                if (c == '{') {
                    brackets++;
                }
                if (c == '}') {
                    brackets--;
                }
                if (brackets <= 0) {
                    break;
                }
            }
        }
        fclose(js);
    }
    write(childfd,"\n",1);
}


void setSQLCA(int code, char *state) {
    if (sqlcode != NULL) {
        cob_field sqlstate = { 5, sqlcode->data+119, NULL };
        cob_set_int(sqlcode,code);
        cob_put_picx(sqlstate.data,sqlstate.size,state);
    }
}


// Callback handler for EXEC statements
int processCmd(char *cmd, cob_field **outputVars) {
    char *pos;
    if ((pos=strstr(cmd,"EXEC SQL")) != NULL) {
        char *sql = (char*)pos+9;
        PGconn *conn = (PGconn*)pthread_getspecific(connKey);
        setSQLCA(0,"00000");
        if (outputVars[0] == NULL) {
            int r = execSQL(conn, sql);
            if (r == 0) {
                setSQLCA(-1,"00000");
            }
        } else {
            // Query returns data
            PGresult *res = execSQLQuery(conn, sql);
            if (res != NULL) {
                int i = 0;
                int cols = PQnfields(res);
                int rows = PQntuples(res);
                if (rows > 0) {
                    while (outputVars[i] != NULL) {
                        if (i < cols) {
                            if (outputVars[i]->attr->type == COB_TYPE_GROUP) {
                                // Map VARCHAR to group struct
                                char *v = (char*)PQgetvalue(res, 0, i);
                                unsigned int l = (unsigned int)strlen(v);
		                if (l > (outputVars[i]->size-2)) {
                                   l = outputVars[i]->size-2;
                                }
	                        outputVars[i]->data[0] = (unsigned char)((l >> 8) & 0xFF);
	                        outputVars[i]->data[1] = (unsigned char)(l & 0xFF);
                                memcpy(&outputVars[i]->data[2],v,l);
                            } else 
                            if (outputVars[i]->attr->type == COB_TYPE_NUMERIC) {
                                char buf[256];
                                cob_put_picx(outputVars[i]->data,outputVars[i]->size,
                                convertNumeric(PQgetvalue(res, 0, i),
                                                 outputVars[i]->attr->digits,
                                                 outputVars[i]->attr->scale,buf));
                            } else
                            if (outputVars[i]->attr->type == COB_TYPE_NUMERIC_PACKED) {
                                long v = atol(PQgetvalue(res, 0, i));
                                cob_put_s64_comp3(v,outputVars[i]->data,outputVars[i]->size);
                            } else 
                            if (getCobType(outputVars[i]) == COB_TYPE_NUMERIC_BINARY) {
                                long v = atol(PQgetvalue(res, 0, i));
                                cob_put_u64_compx(v,outputVars[i]->data,outputVars[i]->size);  
                            } else
                            if (getCobType(outputVars[i]) == COB_TYPE_NUMERIC_COMP5) {
                                long v = atol(PQgetvalue(res, 0, i));
                                cob_put_s64_comp5(v,outputVars[i]->data,outputVars[i]->size);  
                            } else {
                                cob_put_picx(outputVars[i]->data,outputVars[i]->size,PQgetvalue(res, 0, i));
                            }
                        }
                        i++;
                    }
                } else {
                    setSQLCA(100,"02000");
                }
                PQclear(res);
            } else {
                setSQLCA(-1,"00000");
            }
        }
        printf("%s\n",sql);
    }
    return 1;
}


void initMain() {
  void **allocMem = (void**)pthread_getspecific(allocMemKey);
  int *allocMemPtr = (int*)pthread_getspecific(allocMemPtrKey);
  (*allocMemPtr) = 0;
}


void *getmain(int length, int shared) {
  void **allocMem;
  int *allocMemPtr;
  if (shared == 0) {
    allocMem = (void**)pthread_getspecific(allocMemKey);
    allocMemPtr = (int*)pthread_getspecific(allocMemPtrKey);
  } else {
    cm(pthread_mutex_lock(&sharedMemMutex));
    allocMem = sharedAllocMem;
    allocMemPtr = sharedAllocMemPtr;
  }
printf("getmain %d %d %d %d %x\n",length,shared,*allocMemPtr,MEM_POOL_SIZE,allocMem);
  int i = 0;
  for (i = 0; i < (*allocMemPtr); i++) {
      if (allocMem[i] == NULL) {
        break;
      }
  }
  if (i < MEM_POOL_SIZE) {
    void *p = NULL;
    if (shared) {
      p = sharedMalloc(0,length);
      sharedAllocMemLen[i] = length;
    } else {
      p = malloc(length);
    }
    if (p != NULL) {
      allocMem[i] = p;
      if (i == (*allocMemPtr)) {
        (*allocMemPtr)++;
      }
    }
    printf("%s %d %lx %d\n","getmain",length,(unsigned long)p,shared);
    if (shared) {
      cm(pthread_mutex_unlock(&sharedMemMutex));
    }
    return p;
  }
  if (shared) {
    cm(pthread_mutex_unlock(&sharedMemMutex));
  }
  return NULL;
}


int freemain(void *p) {
  void **allocMem = (void**)pthread_getspecific(allocMemKey);
  int *allocMemPtr = (int*)pthread_getspecific(allocMemPtrKey);
  for (int i = 0; i < (*allocMemPtr); i++) {
      if ((p != NULL) && (allocMem[i] == p)) {
          printf("%s %lx\n","freemain",(unsigned long)p);
          free(allocMem[i]);
          allocMem[i] = NULL;
          if (i == (*allocMemPtr)-1) {
            (*allocMemPtr)--;
          }
          return 0;
      }
  }
  // Free shared mem
  int r = -1;
  cm(pthread_mutex_lock(&sharedMemMutex));
  allocMem = sharedAllocMem;
  allocMemPtr = sharedAllocMemPtr;
  for (int i = 0; i < (*allocMemPtr); i++) {
      if ((p != NULL) && (allocMem[i] == p)) {
          printf("%s %lx\n","freemain shared",(unsigned long)p);
          sharedFree(allocMem[i],sharedAllocMemLen[i]);
          allocMem[i] = NULL;
          if (i == (*allocMemPtr)-1) {
            (*allocMemPtr)--;
          }
          r = 0;
          break;
      }
  }
  cm(pthread_mutex_unlock(&sharedMemMutex));
  return r;  
}


void clearMain() {
  void **allocMem = (void**)pthread_getspecific(allocMemKey);
  int *allocMemPtr = (int*)pthread_getspecific(allocMemPtrKey);
  // Clean up, avoid memory leaks
  for (int i = 0; i < (*allocMemPtr); i++) {
      if (allocMem[i] != NULL) {
          free(allocMem[i]);
      }
  }
  (*allocMemPtr) = 0;
}


// Execute SQL pure instruction
void _execSql(char *sql, void *fd, int sendRes, int sync) {
    char response[1024];
    pthread_setspecific(childfdKey, fd);
    if (strstr(sql,"BEGIN")) {
        if (!sync) {
           PGconn *conn = getDBConnection();
           pthread_setspecific(connKey, (void*)conn);
        } else {
           PGconn *conn = (PGconn*)pthread_getspecific(connKey);
           beginDBConnection(conn);            
        }
        return;
    }
    if (strstr(sql,"COMMIT")) {
        PGconn *conn = (PGconn*)pthread_getspecific(connKey);
        int r = 0;
        if (!sync) {
           r = returnDBConnection(conn, 1);
        } else {
           r = syncDBConnection(conn, 1);
        }
        if (sendRes == 1) {
            if (r == 0) {
                sprintf(response,"%s\n","ERROR");
                write(*((int*)fd),&response,strlen(response));
            } else {
                sprintf(response,"%s\n","OK");
                write(*((int*)fd),&response,strlen(response));
            }
        }
        return;
    }
    if (strstr(sql,"ROLLBACK")) {
        PGconn *conn = (PGconn*)pthread_getspecific(connKey);
        int r = 0;
        if (!sync) {
           r = returnDBConnection(conn, 0);
        } else {
           r = syncDBConnection(conn, 0);
        }
        if (sendRes == 1) {
            if (r == 0) {
                sprintf(response,"%s\n","ERROR");
                write(*((int*)fd),&response,strlen(response));
            } else {
                sprintf(response,"%s\n","OK");
                write(*((int*)fd),&response,strlen(response));
            }
        }
        return;
    }
    if ((strstr(sql,"SELECT") || strstr(sql,"FETCH") || strstr(sql,"select") || strstr(sql,"fetch")) &&
        (strstr(sql,"DECLARE") == NULL) && (strstr(sql,"declare") == NULL)) {
        PGconn *conn = (PGconn*)pthread_getspecific(connKey);
        PGresult *res = execSQLQuery(conn, sql);
        if (res != NULL) {
            int i,j;
            int cols = PQnfields(res);
            int rows = PQntuples(res);
            sprintf(response,"%s\n","OK");
            write(*((int*)fd),&response,strlen(response));
            sprintf(response,"%d\n",cols);
            write(*((int*)fd),&response,strlen(response));
            for (j = 0; j < cols; j++) {
                sprintf(response,"%s\n",PQfname(res,j));
                write(*((int*)fd),&response,strlen(response));
            }
            sprintf(response,"%d\n",rows);
            write(*((int*)fd),&response,strlen(response));
            for (i = 0; i < rows; i++) {
                for (j = 0; j < cols; j++) {
                    sprintf(response,"%s\n",PQgetvalue(res, i, j));
                    write(*((int*)fd),&response,strlen(response));
                }
            }
            PQclear(res);
        } else {
            sprintf(response,"%s\n","ERROR");
            write(*((int*)fd),&response,strlen(response));
        }
        return;
    }    
    PGconn *conn = (PGconn*)pthread_getspecific(connKey);
    char *r = execSQLCmd(conn, sql);
    if (r == NULL) {
        sprintf(response,"%s\n","ERROR");
        write(*((int*)fd),&response,strlen(response));
    } else {
        sprintf(response,"%s%s\n","OK:",r);
        write(*((int*)fd),&response,strlen(response));
    }
}


int setJmpAbend(int *errcond, char *bufVar) {
  jmp_buf *h = condHandler[*errcond];
  if (h == NULL) {
      h = malloc(sizeof(jmp_buf));
      condHandler[*errcond] = h;
  }
  memcpy(h,bufVar,sizeof(jmp_buf));
  return 0;
}


void abend(int resp, int resp2) {
  char response[1024];
  char *abcode = "ASRA";
  switch (resp) {
      case 16: abcode = "A47B"; 
               break;
      case 22: abcode = "AEIV"; 
               break;
      case 23: abcode = "AEIW"; 
               break;
      case 26: abcode = "AEIZ"; 
               break;
      case 27: abcode = "AEI0"; 
               break;
      case 28: abcode = "AEI1"; 
               break;
      case 44: abcode = "AEYH"; 
               break;
      case 55: abcode = "ASRA"; 
               break;
      case 82: abcode = "ASRA"; 
               break;
      case 110: abcode = "ASRA"; 
               break;
      case 122: abcode = "ASRA"; 
               break;
  }
  int *respFieldsState = (int*)pthread_getspecific(respFieldsStateKey);
  int *cmdState = (int*)pthread_getspecific(cmdStateKey);
  if ((*cmdState) != -17) {
    // ABEND not triggered by explicit ABEND command
    if ((*respFieldsState) > 0) {
      // RESP param set, continue
      return;      
    }
    char buf[56];
    int childfd = *((int*)pthread_getspecific(childfdKey));
    sprintf(buf,"%s","ABEND\n");
    write(childfd,buf,strlen(buf));
    sprintf(buf,"%s","ABCODE\n");
    write(childfd,buf,strlen(buf));
    sprintf(buf,"%s%s%s","='",abcode,"'\n\n");
    write(childfd,buf,strlen(buf));

    int *runState = (int*)pthread_getspecific(runStateKey);
    if ((*runState) == 3) {   // SEGV ABEND
        sprintf(response,"\n%s\n","STOP");
        write(childfd,&response,strlen(response));
    }
  }
  fprintf(stderr,"%s%s%s%d%s%d\n","ABEND ABCODE=",abcode," RESP=",resp," RESP2=",resp2);
  jmp_buf *h = condHandler[resp];
  if (h != NULL) {
    longjmp(*h,1);
  } else {
    longjmp(taskState,1);
  }
}


void readLine(char *buf, int childfd) {
  buf[0] = 0x00;
  char c = 0x00;
  int pos = 0;
  while (c != '\n') {
      int n = read(childfd,&c,1);
      if ((n == 1) && (pos < 2047) && (c != '\n') && (c != '\r') && (c != '\'')) {
          buf[pos] = c;
          pos++;
      }
  }
  buf[pos] = 0x00;
}


// Handling plain COBOL call invocation for preprocessed QWICS modules
struct callLoadlib {
    char name[9];
    void* sdl_library;
    int (*loadmod)();    
};


// Db2 DSNTIAR assembler routine mockup
int dsntiar(unsigned char *commArea, unsigned char *sqlca, unsigned char *errMsg, int32_t *errLen) {
    return 0;
}


// EXEC XML GENERATE replacement
int xmlGenerate(unsigned char *xmlOutput, unsigned char *sourceRec, int32_t *xmlCharCount) {
    int childfd = *((int*)pthread_getspecific(childfdKey));
    char *commArea = (char*)pthread_getspecific(commAreaKey);

    write(childfd,"XML\n",4);
    write(childfd,"GENERATE\n",9);
    write(childfd,"SOURCE-REC\n",11);
    write(childfd,"XML-CHAR-COUNT\n",15);
    char lbuf[32];
    sprintf(lbuf,"%s%d\n","=",(int)*xmlCharCount);
    write(childfd,lbuf,strlen(lbuf));
    write(childfd,"\n",1);

    char c = 0x00;
    int pos = 0;
    while (pos < (int)(*xmlCharCount)) {
      int n = read(childfd,&c,1);
      if (n == 1) {
          xmlOutput[pos] = c;
          pos++;
      }    
    }

    char buf[2048];
    readLine((char*)&buf,childfd);
    int res = atoi(buf);
    readLine((char*)&buf,childfd);
    return res;
}


void* globalCallCallback(char *name) {
    void *res = NULL;
    char fname[255];
    char response[1024];
    int *callStackPtr = (int*)pthread_getspecific(callStackPtrKey);
    struct callLoadlib *callStack = (struct callLoadlib *)pthread_getspecific(callStackKey);
    int i = 0;

    #ifdef __APPLE__
    sprintf(fname,"%s%s%s%s",GETENV_STRING(loadmodDir,"QWICS_LOADMODDIR","../loadmod"),"/",name,".dylib");
    #else
    sprintf(fname,"%s%s%s%s",GETENV_STRING(loadmodDir,"QWICS_LOADMODDIR","../loadmod"),"/",name,".so");
    #endif
printf("%s\n",fname);

    if (strcmp("DSNTIAR",name) == 0) {
        return (void*)&dsntiar;
    }

    if (strcmp("xmlGenerate",name) == 0) {
        return (void*)&xmlGenerate;
    }

    for (i = 0; i < (*callStackPtr); i++) {
        if (strcmp(name,callStack[i].name) == 0) {
            return (void*)callStack[i].loadmod;
        }
    } 

    callStack[*callStackPtr].sdl_library = dlopen(fname, RTLD_LAZY);
    if (callStack[*callStackPtr].sdl_library == NULL) {
        sprintf(response,"%s%s%s\n","ERROR: Load module ",fname," not found!");
        printf("%s",response);
    } else {
        dlerror();
        *(void**)(&callStack[*callStackPtr].loadmod) = dlsym(callStack[*callStackPtr].sdl_library,name);
        char *error;
        if ((error = dlerror()) != NULL)  {
            dlclose(callStack[*callStackPtr].sdl_library);
            sprintf(response,"%s%s\n","ERROR: ",error);
            printf("%s",response);
            abend(27,1);
        } else {
            sprintf(callStack[*callStackPtr].name,"%s",name);
            res = (void*)callStack[*callStackPtr].loadmod;

            if (*callStackPtr < 1023) {
                (*callStackPtr)++;
            }
        }
    }

    return res; 
}


void globalCallCleanup() {
    int *callStackPtr = (int*)pthread_getspecific(callStackPtrKey);
    struct callLoadlib *callStack = (struct callLoadlib *)pthread_getspecific(callStackKey);

    int i = 0;
    for (i = (*callStackPtr)-1; i >= 0; i--) {
        dlclose(callStack[i].sdl_library);
    } 
    *callStackPtr = 0;
}


// Execute COBOL loadmod in transaction
int execLoadModule(char *name, int mode, int parCount) {
    int (*loadmod)();
    char fname[255];
    char response[1024];
    int childfd = *((int*)pthread_getspecific(childfdKey));
    char *commArea = (char*)pthread_getspecific(commAreaKey);
    int res = 0;

    #ifdef __APPLE__
    sprintf(fname,"%s%s%s%s",GETENV_STRING(loadmodDir,"QWICS_LOADMODDIR","../loadmod"),"/",name,".dylib");
    #else
    sprintf(fname,"%s%s%s%s",GETENV_STRING(loadmodDir,"QWICS_LOADMODDIR","../loadmod"),"/",name,".so");
    #endif
    void* sdl_library = dlopen(fname, RTLD_LAZY);
    if (sdl_library == NULL) {
        sprintf(response,"%s%s%s\n","ERROR: Load module ",fname," not found!");
        if (mode == 0) {
            write(childfd,&response,strlen(response));
        }
        printf("%s",response);
        res = -1;
    } else {
        dlerror();
        *(void**)(&loadmod) = dlsym(sdl_library,name);
        char *error;
        if ((error = dlerror()) != NULL)  {
            sprintf(response,"%s%s\n","ERROR: ",error);
            if (mode == 0) {
                write(childfd,&response,strlen(response));
            }
            printf("%s",response);
            res = -2;
            if (mode == 1) {
              abend(27,1);
            }
        } else {
            if (mode == 0) {
                sprintf(response,"%s\n","OK");
                write(childfd,&response,strlen(response));
            }
#ifndef _USE_ONLY_PROCESSES_
            startModule(name);
#endif
            if (mode == 0) {
              if (setjmp(taskState) == 0) {
                if (parCount > 0) {
                    if (parCount == 1) (*loadmod)(commArea,paramList[0]);
                    if (parCount == 2) (*loadmod)(commArea,paramList[0],paramList[1]);
                    if (parCount == 3) (*loadmod)(commArea,paramList[0],paramList[1],paramList[2]);
                    if (parCount == 4) (*loadmod)(commArea,paramList[0],paramList[1],paramList[2],paramList[3]);
                    if (parCount == 5) (*loadmod)(commArea,paramList[0],paramList[1],paramList[2],paramList[3],paramList[4]);
                    if (parCount == 6) (*loadmod)(commArea,paramList[0],paramList[1],paramList[2],paramList[3],paramList[4],
                                                            paramList[5]);
                    if (parCount == 7) (*loadmod)(commArea,paramList[0],paramList[1],paramList[2],paramList[3],paramList[4],
                                                            paramList[5],paramList[6]);
                    if (parCount == 8) (*loadmod)(commArea,paramList[0],paramList[1],paramList[2],paramList[3],paramList[4],
                                                            paramList[5],paramList[6],paramList[7]);
                    if (parCount == 9) (*loadmod)(commArea,paramList[0],paramList[1],paramList[2],paramList[3],paramList[4],
                                                            paramList[5],paramList[6],paramList[7],paramList[8]);
                    if (parCount == 10) (*loadmod)(commArea,paramList[0],paramList[1],paramList[2],paramList[3],paramList[4],
                                                            paramList[5],paramList[6],paramList[7],paramList[8],paramList[9]);
                } else {
                    (*loadmod)(commArea);
                }
              }
            } else {
              cob_get_global_ptr()->cob_current_module = &thisModule;
              cob_get_global_ptr()->cob_call_params = 1;
              (*loadmod)(commArea);
            }
#ifndef _USE_ONLY_PROCESSES_
            endModule(name);
#endif
            int *runState = (int*)pthread_getspecific(runStateKey);
            if ((mode == 0) && ((*runState) < 3)) {
                sprintf(response,"\n%s\n","STOP");
                write(childfd,&response,strlen(response));
            }
        }
        dlclose(sdl_library);
    }
    return res;
}


int execCallback(char *cmd, void *var) {
    int childfd = *((int*)pthread_getspecific(childfdKey));
    char *cmdbuf = (char*)pthread_getspecific(cmdbufKey);
    int *cmdState = (int*)pthread_getspecific(cmdStateKey);
    int *runState = (int*)pthread_getspecific(runStateKey);
    cob_field **outputVars = (cob_field**)pthread_getspecific(cobFieldKey);
    char *end = &cmdbuf[strlen(cmdbuf)];
    int *xctlState = (int*)pthread_getspecific(xctlStateKey);
    int *retrieveState = (int*)pthread_getspecific(retrieveStateKey);
    char **xctlParams = (char**)pthread_getspecific(xctlParamsKey);
    char *eibbuf = (char*)pthread_getspecific(eibbufKey);
    char *linkArea = (char*)pthread_getspecific(linkAreaKey);
    int *linkAreaPtr = (int*)pthread_getspecific(linkAreaPtrKey);
    char **linkAreaAdr = (char**)pthread_getspecific(linkAreaAdrKey);
    char *commArea = (char*)pthread_getspecific(commAreaKey);
    int *commAreaPtr = (int*)pthread_getspecific(commAreaPtrKey);
    int *areaMode = (int*)pthread_getspecific(areaModeKey);
    char *linkStack = (char*)pthread_getspecific(linkStackKey);
    int *linkStackPtr = (int*)pthread_getspecific(linkStackPtrKey);
    void **memParams = (void**)pthread_getspecific(memParamsKey);
    int *memParamsState = (int*)pthread_getspecific(memParamsStateKey);
    char *twa = (char*)pthread_getspecific(twaKey);
    char *tua = (char*)pthread_getspecific(tuaKey);
    int *respFieldsState = (int*)pthread_getspecific(respFieldsStateKey);
    void **respFields = (void**)pthread_getspecific(respFieldsKey);
    int *callStackPtr = (int*)pthread_getspecific(callStackPtrKey);
    int respFieldsStateLocal = 0;
    void *respFieldsLocal[2];

    struct taskLock *taskLocks = (struct taskLock *)pthread_getspecific(taskLocksKey);

    // printf("%s %s %d %d %x\n","execCallback",cmd,*cmdState,*memParamsState,var);

    if (strstr(cmd,"SET SQLCODE") && (var != NULL)) {
        sqlcode = var;
        return 1;
    }
    if (strstr(cmd,"SET EIBCALEN") && (((*linkStackPtr) == 0) && ((*callStackPtr) == 0))) {
        cob_field *cobvar = (cob_field*)var;
        // Read in client response value
        char buf[2048];
        buf[0] = 0x00;
        char c = 0x00;
        int pos = 0;
        while (c != '\n') {
            int n = read(childfd,&c,1);
            if ((n == 1) && (pos < 2047) && (c != '\n') && (c != '\r') && (c != '\'')) {
                buf[pos] = c;
                pos++;
            }
        }
        buf[pos] = 0x00;
        long val = (long)atol(buf);
        cob_put_u64_compx(val,cobvar->data,(size_t)cobvar->size);
        return 1;
    }
    if (strstr(cmd,"SET EIBAID") && (((*linkStackPtr) == 0) && ((*callStackPtr) == 0))) {
        (*commAreaPtr) = 0;
        (*areaMode) = 0;
        // Handle EIBAID
        cob_field *cobvar = (cob_field*)var;
        // Read in client response value
        char buf[2048];
        buf[0] = 0x00;
        char c = 0x00;
        int pos = 0;
        while (c != '\n') {
            int n = read(childfd,&c,1);
            if ((n == 1) && (pos < 2047) && (c != '\n') && (c != '\r') && (c != '\'')) {
                buf[pos] = c;
                pos++;
            }
        }
        buf[pos] = 0x00;
        cob_put_picx(cobvar->data,(size_t)cobvar->size,buf);
        return 1;
    }
    if (strstr(cmd,"SET DFHEIBLK") && (((*linkStackPtr) == 0) && ((*callStackPtr) == 0))) {
        cob_field *cobvar = (cob_field*)var;
        if (cobvar->data != NULL) {
            eibbuf = (char*)cobvar->data;
            pthread_setspecific(eibbufKey, eibbuf);
        }
        // Read in TRNID from client
        char c = 0x00;
        int pos = 8;
        while (c != '\n') {
            int n = read(childfd,&c,1);
            if ((n == 1) && (pos < 12) && (c != '\n') && (c != '\r') && (c != '\'')) {
                eibbuf[pos] = c;
                pos++;
            }
        }
        while (pos < 12) {
            eibbuf[pos] = ' ';
            pos++;
        }
        // Read in REQID from client
        c = 0x00;
        pos = 43;
        while (c != '\n') {
            int n = read(childfd,&c,1);
            if ((n == 1) && (pos < 51) && (c != '\n') && (c != '\r') && (c != '\'')) {
                eibbuf[pos] = c;
                pos++;
            }
        }
        while (pos < 51) {
            eibbuf[pos] = ' ';
            pos++;
        }
        // Read in TERMID from client
        c = 0x00;
        pos = 16;
        while (c != '\n') {
            int n = read(childfd,&c,1);
            if ((n == 1) && (pos < 20) && (c != '\n') && (c != '\r') && (c != '\'')) {
                eibbuf[pos] = c;
                pos++;
            }
        }
        while (pos < 20) {
            eibbuf[pos] = '0';
            pos++;
        }
        // Read in TASKID from client
        char idbuf[9];
        c = 0x00;
        pos = 0;
        while (c != '\n') {
            int n = read(childfd,&c,1);
            if ((n == 1) && (pos < 8) && (c != '\n') && (c != '\r') && (c != '\'')) {
                idbuf[pos] = c;
                pos++;
            }
        }
        idbuf[pos] = 0x00;
        int id = atoi(idbuf);
        cob_put_s64_comp3(id,(void*)&eibbuf[12],4);
        // SET EIBDATE and EIBTIME
        time_t t = time(NULL);
        struct tm now = *localtime(&t);
        int ti = now.tm_hour*10000 + now.tm_min*100 + now.tm_sec;
        cob_put_s64_comp3(ti,(void*)&eibbuf[0],4);
        int da = now.tm_year*1000 + now.tm_yday;
        cob_put_s64_comp3(da,(void*)&eibbuf[4],4);
        return 1;
    } else 
    if (strstr(cmd,"SET DFHEIBLK") && (((*linkStackPtr) >= 0) || ((*callStackPtr) >= 0))) {
        // Called by LINk inside transaction, pass through EIB
        cob_field *cobvar = (cob_field*)var;
        if (cobvar->data != NULL) {
            int n = 0;
            for (n = 0; n < cobvar->size; n++) {
                ((char*)cobvar->data)[n] = eibbuf[n];
            }
        }
        return 1;
    }

    if (strstr(cmd,"SETL1 1 ") || strstr(cmd,"SETL0 1 ") || strstr(cmd,"SETL0 77")) {
        (*areaMode) = 0;
    }
    if (strstr(cmd,"DFHCOMMAREA")) {
        (*areaMode) = 1;
    }
    if (strstr(cmd,"SETL0") || strstr(cmd,"SETL1")) {
        cob_field *cobvar = (cob_field*)var;
        if ((*areaMode) == 0) {
            if (strstr(cmd,"SETL1 1 ") || strstr(cmd,"SETL0 1 ") || strstr(cmd,"SETL0 77")) {
                // Top level var
                // printf("data %x\n",cobvar->data);
                if (cobvar->data == NULL) {
                    cobvar->data = (unsigned char*)&linkArea[*linkAreaPtr];
                    (*linkAreaAdr) = &linkArea[*linkAreaPtr];
                    (*linkAreaPtr) += (size_t)cobvar->size;
                    // printf("set top level linkAreaPtr %x\n",cobvar->data);
                }
            } else {
                // printf("linkAreaPtr = %d\n",*linkAreaPtr);
                if ((unsigned long)(*linkAreaAdr) + (unsigned long)cobvar->data < (unsigned long)&linkArea[*linkAreaPtr]) {
                    cobvar->data = (unsigned char*)(*linkAreaAdr) + (unsigned long)cobvar->data;
                    // printf("set sub level linkAreaPtr %x\n",cobvar->data);
                }
            }
        } else {
            if (cobvar->data == NULL) {
                cobvar->data = (unsigned char*)&commArea[*commAreaPtr];
                (*commAreaPtr) += (size_t)cobvar->size;
            }
        }

        if ((strstr(cmd,"SETL0 77") || strstr(cmd,"SETL1 1 ")) && 
            (((*linkStackPtr) == 0) && ((*callStackPtr) == 0)) && ((*areaMode) == 0)) {
/*
            cob_field *cobvar = (cob_field*)var;
            char obuf[255];
            sprintf(obuf,"%s %ld\n",cmd,cobvar->size);
            write(childfd,obuf,strlen(obuf));
*/
            // Read in value from client
/*        
            char lvar[65536];
            char c = 0x00;
            int pos = 0;
            while (c != '\n') {
                int n = read(childfd,&c,1);
                if ((n == 1) && (c != '\n') && (c != '\r') && (c != '\'') && (pos < 65536)) {
                    lvar[pos] = c;
                    pos++;
                }
            }
            lvar[pos] = 0x00; 

            char buf[256];
            long v = 0;
            switch (COB_FIELD_TYPE(cobvar)) {
                case COB_TYPE_NUMERIC:          cob_put_picx(cobvar->data,cobvar->size,
                                                            convertNumeric(lvar,cobvar->attr->digits,cobvar->attr->scale,buf));
                                                break;
                case COB_TYPE_NUMERIC_BINARY:   v = atol(lvar);
                                                cob_put_u64_compx(v,cobvar->data,cobvar->size);
                                                break;
                case COB_TYPE_NUMERIC_PACKED:   v = atol(lvar);
                                                cob_put_s64_comp3(v,cobvar->data,cobvar->size);                     
                                                break;
                default:                        cob_put_picx(cobvar->data,(size_t)cobvar->size,lvar);
            }
 */       
        }
        return 1;
    }

    if (strcmp(cmd,"CICS") == 0) {
        cmdbuf[0] = 0x00;
        (*cmdState) = -1;
        return 1;
    }

    if ((*cmdState) < 0) {
        if (strcmp(cmd,"SEND") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"RECEIVE") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -2;
            (*memParamsState) = 0;
            (*respFieldsState) = 0;
            *((int*)memParams[0]) = -1;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"XCTL") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -3;
            (*xctlState) = 0;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"RETRIEVE") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -4;
            (*retrieveState) = 0;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"LINK") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -5;
            (*xctlState) = 0;
            xctlParams[1] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if ((strcmp(cmd,"GETMAIN") == 0) || (strcmp(cmd,"GETMAIN64") == 0)) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -6;
            (*memParamsState) = 0;
            memParams[2] = (void*)0;
            memParams[3] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if ((strcmp(cmd,"FREEMAIN") == 0) || (strcmp(cmd,"FREEMAIN64") == 0)) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -7;
            (*memParamsState) = 0;
            memParams[2] = (void*)0; // SHARED
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"ADDRESS") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -8;
            (*memParamsState) = 0;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"PUT") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -9;
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"GET") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -10;
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            memParams[1] = NULL;
            memParams[2] = NULL;
            memParams[3] = NULL;
            memParams[4] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"ENQ") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -11;
            (*memParamsState) = 0;
            (*((int*)memParams[0])) = -1;
            memParams[2] = NULL;
            memParams[3] = NULL;
            memParams[4] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"DEQ") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -12;
            (*memParamsState) = 0;
            (*((int*)memParams[0])) = -1;
            memParams[2] = NULL;
            memParams[3] = NULL;
            memParams[4] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"SYNCPOINT") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -13;
            (*memParamsState) = 0;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"WRITEQ") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -14;
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            memParams[3] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"READQ") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -15;
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            memParams[3] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"DELETEQ") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -16;
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"ABEND") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -17;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if ((strcmp(cmd,"ASKTIME") == 0) ||
            (strcmp(cmd,"INQUIRE") == 0) ||
            (strcmp(cmd,"ASSIGN") == 0) ||
            (strcmp(cmd,"FORMATTIME") == 0)) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -18; // General read only data cmd
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if ((strcmp(cmd,"START") == 0) ||
            (strcmp(cmd,"CANCEL") == 0)) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -19; // Call other transactions
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            memParams[1] = NULL;
            memParams[2] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"RETURN") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -20; // RETURN
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            memParams[1] = NULL;
            memParams[2] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            (*runState) = 2; // TASK ENDED
            return 1;
        }
        if (strcmp(cmd,"SOAPFAULT") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -21;
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"INVOKE") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -22;
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }
        if (strcmp(cmd,"QUERY") == 0) {
            sprintf(cmdbuf,"%s%s",cmd,"\n");
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            (*cmdState) = -23;
            (*memParamsState) = 0;
            *((int*)memParams[0]) = -1;
            memParams[1] = NULL;
            memParams[2] = NULL;
            memParams[3] = NULL;
            memParams[4] = NULL;
            (*respFieldsState) = 0;
            respFields[0] = NULL;
            respFields[1] = NULL;
            return 1;
        }

        if (strstr(cmd,"END-EXEC")) {
            int resp = 0;
            int resp2 = 0;
            cmdbuf[0] = 0x00;
            outputVars[0] = NULL; // NULL terminated list
            write(childfd,"\n",1);
            if (((*cmdState) == -2) && ((*memParamsState) >= 1)) {
                int len = *((int*)memParams[0]);
                cob_field *cobvar = (cob_field*)memParams[1];
                int i,l;
                if ((len >= 0) && (len <= cobvar->size)) {
                  l = len;
                } else {
                  l = cobvar->size;
                }
                char c;
                i = 0;
                while (i < l) {
                    int n = read(childfd,&c,1);
                    if (n == 1) {
                        cobvar->data[i] = c;
                        i++;
                    }
                }
                while (i < cobvar->size) {
                  int n = read(childfd,&c,1);
                  if (n == 1) {
                      i++;
                  }
                }

                char buf[2048];
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                if (resp > 0) {
                  abend(resp,resp2);
                }
            }
            if (((*cmdState) == -3) && ((*xctlState) >= 1)) {
                // XCTL
                (*xctlState) = 0;
                (*cmdState) = 0;
                //printf("%s%s\n","XCTL ",xctlParams[0]);
                execLoadModule(xctlParams[0],1,0);
            }
            if (((*cmdState) == -4) && ((*retrieveState) >= 1)) {
                // RETRIEVE
                (*retrieveState) = 0;

                char buf[2048];
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                if (resp > 0) {
                  abend(resp,resp2);
                }
            }
            if (((*cmdState) == -5) && ((*xctlState) >= 1)) {
                // LINK
                (*xctlState) = 0;
                (*cmdState) = 0;
                sprintf(&(linkStack[9*(*linkStackPtr)]),"%s",xctlParams[0]);
                if ((*linkStackPtr) < 99) {
                  (*linkStackPtr)++;
                }
                if (xctlParams[1] != NULL) {
                    cob_field *cobvar = (cob_field*)xctlParams[1];
                    if (((int)cobvar->size < 0) || (cobvar->size > 32768)) {
                        resp = 22;
                        resp2 = 11;
                    }
                }
                if (resp == 0) {
                    respFieldsStateLocal = *respFieldsState;
                    respFieldsLocal[0] = respFields[0];
                    respFieldsLocal[1] = respFields[1];
                    cob_field *cobvar = (cob_field*)xctlParams[1];

                    int r = execLoadModule(xctlParams[0],1,0);

                    *respFieldsState = respFieldsStateLocal;
                    respFields[0] = respFieldsLocal[0];
                    respFields[1] = respFieldsLocal[1];
                    *cmdState = -5;

                    if (r < 0) {
                        resp = 27;
                        resp2 = 3;
                    }
                    if ((resp == 0) && (cobvar != NULL)) {
                        for (int i = 0; i < cobvar->size; i++) {
                            cobvar->data[i] = commArea[i];
                        }
                    }
                }
                if ((*linkStackPtr) > 0) {
                  (*linkStackPtr)--;
                }
                if (resp > 0) {
                  abend(resp,resp2);
                }
            }
            if (((*cmdState) == -6) && ((*memParamsState) >= 1)) {
                cob_field *cobvar = (cob_field*)memParams[1];
                if (*((int*)memParams[0]) < 1) {
                  resp = 22;
                }
                (*((unsigned char**)cobvar->data)) = (unsigned char*)getmain(*((int*)memParams[0]),(int)memParams[2]);
                if (cobvar->data == NULL) {
                  resp = 22;
                }
                if (resp > 0) {
                  abend(resp,resp2);
                }
                if (memParams[3] != NULL) {
                  // INITIMG
                  for (int i = 0; i < *((int*)memParams[0]); i++) {
                    (*((unsigned char**)cobvar->data))[i] = ((char*)memParams[3])[0];
                  }
                }
            }
            if (((*cmdState) == -7) && ((*memParamsState) >= 1)) {
                if (freemain(memParams[1]) < 0) {
                    resp = 16;
                    resp2 = 1;
                    abend(resp,resp2);
                }
            }
            if (((*cmdState) == -9) && ((*memParamsState) >= 1)) {
                int len = *((int*)memParams[0]);
                cob_field *cobvar = (cob_field*)memParams[1];
                int i,l;
                if ((len >= 0) && (len <= cobvar->size)) {
                  l = len;
                } else {
                  l = cobvar->size;
                }
                write(childfd,cobvar->data,l);
                if (l < len) {
                  char zero[1];
                  zero[0] = 0x00;
                  for (i = l; i < len; i++) {
                    write(childfd,&zero,1);
                  }
                }
                char buf[2048];
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                write(childfd,"\n",1);
                write(childfd,"\n",1);
            }
            if (((*cmdState) == -10) && ((*memParamsState) >= 1)) {
                char buf[2048];
                int len = *((int*)memParams[0]);
                cob_field *cobvar = NULL, dummy = { len, NULL, NULL };
                if (memParams[1] != NULL) {
                    cobvar = (cob_field*)memParams[1];
                } 
                if (memParams[2] != NULL) {
                    // SET mode
                    readLine((char*)&buf,childfd);
                    len = atoi(buf);

                    (*((unsigned char**)((cob_field*)memParams[2])->data)) = getNextChnBuf(len);
                    dummy.size = len;
                    dummy.data = (*((unsigned char**)((cob_field*)memParams[2])->data));
                    cobvar = &dummy;
                }     
                if (memParams[4] != NULL) {
                    // NODATA mode
                    readLine((char*)&buf,childfd);
                    len = atoi(buf);
                    dummy.size = len;
                    cobvar = &dummy;                    
                }
                int i,l = 0;
                if (cobvar != NULL) {
                    if ((len >= 0) && (len <= cobvar->size)) {
                        l = len;
                    } else {
                        l = cobvar->size;
                    }
                }
                if (memParams[3] != NULL) {
                    if (((cob_field*)memParams[3])->data != NULL) {
                        setNumericValue(l,(cob_field*)memParams[3]);                        
                    }
                }
                if (memParams[4] != NULL) {
                    // NODATA mode
                    l = 0;
                    len = 0;
                }
                char c;
                i = 0;
                while (i < l) {
                    int n = read(childfd,&c,1);
                    if (n == 1) {
                        cobvar->data[i] = c;
                        i++;
                    }
                }
                while (i < len) {
                  int n = read(childfd,&c,1);
                  if (n == 1) {
                      i++;
                  }
                }

                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
            }
            if (((*cmdState) == -11) && ((*memParamsState) >= 1)) {
                int len = *((int*)memParams[0]);
                cob_field *cobvar = (cob_field*)memParams[1];
                int type = ((int)memParams[4] == 1) ? 1 : 0;
                int nosuspend = ((int)memParams[2] == 1) ? 1 : 0;
                if (len <= 0) {
                  int r = enq((char*)cobvar,0,nosuspend,type,taskLocks);
                } else {
                  if (len > 255) {
                    resp = 22;
                    resp2 = 1;
                  } else {
                    int r = enq((char*)cobvar->data,len,nosuspend,type,taskLocks);
                    if (r < 0) {
                      resp = 55;
                    }
                  }
                }
            }
            if (((*cmdState) == -12) && ((*memParamsState) >= 1)) {
                int len = *((int*)memParams[0]);
                cob_field *cobvar = (cob_field*)memParams[1];
                int type = ((int)memParams[4] == 1) ? 1 : 0;
                if (len <= 0) {
                  deq((char*)cobvar,0,type,taskLocks);
                } else {
                  if (len > 255) {
                    resp = 22;
                    resp2 = 1;
                  } else {
                    deq((char*)cobvar->data,len,type,taskLocks);
                  }
                }
            }
            if ((*cmdState) == -13) {
                // SYNCPOINT handling
                char buf[2048];
                buf[0] = 0x00;
                while(strstr(buf,"END-SYNCPOINT") == NULL) {
                  char c = 0x00;
                  int pos = 0;
                  while (c != '\n') {
                    int n = read(childfd,&c,1);
                    if ((n == 1) && (pos < 2047) && (c != '\n') && (c != '\r')) {
                      buf[pos] = c;
                      pos++;
                    }
                  }
                  buf[pos] = 0x00;
                  if (pos > 0) {
                    char *cmd = strstr(buf,"sql");
                    if (cmd) {
                      char *sql = cmd+4;
                      _execSql(sql, pthread_getspecific(childfdKey),1,1);
                    }
                  }
                }
                releaseLocks(UOW, taskLocks);
                if ((strstr(buf,"ROLLBACK") != NULL) && ((*memParamsState) == 0)) {
                  if ((*memParamsState) == 0) {
                    resp = 82;
                  }
                  abend(resp,resp2);
                }
            }
            if (((*cmdState) == -14) && ((*memParamsState) >= 1)) {
                int len = *((int*)memParams[0]);
                cob_field *cobvar = (cob_field*)memParams[1];
                int i,l;
                if ((len >= 0) && (len <= cobvar->size)) {
                  l = len;
                } else {
                  l = cobvar->size;
                }
                write(childfd,cobvar->data,l);
                if (l < len) {
                  char zero[1];
                  zero[0] = 0x00;
                  for (i = l; i < len; i++) {
                    write(childfd,&zero,1);
                  }
                }
                char buf[2048];
                readLine((char*)&buf,childfd);
                int item = atoi(buf);
                if (memParams[3] != NULL) {
                    if (getCobType((cob_field*)memParams[3]) == COB_TYPE_NUMERIC_BINARY) {
                        cob_put_u64_compx(item,((cob_field*)memParams[3])->data,2);
                    }
                    if (getCobType((cob_field*)memParams[3]) == COB_TYPE_NUMERIC_COMP5) {
                        cob_put_s64_comp5(item,((cob_field*)memParams[3])->data,2);
                    }
                }
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                write(childfd,"\n",1);
                write(childfd,"\n",1);
                if (resp > 0) {
                  abend(resp,resp2);
                }
            }
            if (((*cmdState) == -15) && ((*memParamsState) >= 1)) {
                int len = *((int*)memParams[0]);
                cob_field *cobvar = (cob_field*)memParams[1];
                int i,l;
                if ((len >= 0) && (len <= cobvar->size)) {
                  l = len;
                } else {
                  l = cobvar->size;
                }
                char c;
                i = 0;
                while (i < l) {
                    int n = read(childfd,&c,1);
                    if (n == 1) {
                        cobvar->data[i] = c;
                        i++;
                    }
                }
                while (i < len) {
                  int n = read(childfd,&c,1);
                  if (n == 1) {
                      i++;
                  }
                }
                char buf[2048];
                readLine((char*)&buf,childfd);
                int item = atoi(buf);
                if (memParams[3] != NULL) {
                    if (getCobType((cob_field*)memParams[3]) == COB_TYPE_NUMERIC_BINARY) {
                        cob_put_u64_compx(item,((cob_field*)memParams[3])->data,2);
                    }
                    if (getCobType((cob_field*)memParams[3]) == COB_TYPE_NUMERIC_COMP5) {
                        cob_put_s64_comp5(item,((cob_field*)memParams[3])->data,2);
                    }
                }
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                if (resp > 0) {
                  abend(resp,resp2);
                  if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) {
                      memset(cobvar->data,' ',cobvar->size);
                  } else {
                      memset(cobvar->data,0x00,cobvar->size);
                  }
                }
            }
            if ((*cmdState) == -16) {
                char buf[2048];
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
            }
            if ((*cmdState) == -17) {
              abend(resp,resp2);
            }
            if ((*cmdState) == -18) {
                char buf[2048];
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
            }
            if ((*cmdState) == -19) {
                // Send FROM data
                int len = *((int*)memParams[0]);
                cob_field *cobvar = (cob_field*)memParams[1];
                int i,l;
                if (cobvar != NULL) {
                  if ((len >= 0) && (len <= cobvar->size)) {
                    l = len;
                  } else {
                    l = cobvar->size;
                  }
                  write(childfd,cobvar->data,l);
                }

                char buf[2048];
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                if (resp > 0) {
                  abend(resp,resp2);
                }
            }
            if ((*cmdState) == -21) {
                char buf[2048];
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                if (resp > 0) {
                  abend(resp,resp2);
                }
            }
            if ((*cmdState) == -22) {
                char buf[2048];
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                if (resp > 0) {
                  abend(resp,resp2);
                }
            }
            if ((*cmdState) == -23) {
                char buf[2048];
                // READ
                readLine((char*)&buf,childfd);
                int v = atoi(buf);
                if (memParams[1] != NULL) {
                    if (((cob_field*)memParams[1])->data != NULL) {
                        setNumericValue(v,(cob_field*)memParams[1]);                        
                    }
                }
                // UPDATE
                readLine((char*)&buf,childfd);
                v = atoi(buf);
                if (memParams[2] != NULL) {
                    if (((cob_field*)memParams[2])->data != NULL) {
                        setNumericValue(v,(cob_field*)memParams[2]);                        
                    }
                }
                // CONTROL
                readLine((char*)&buf,childfd);
                v = atoi(buf);
                if (memParams[3] != NULL) {
                    if (((cob_field*)memParams[3])->data != NULL) {
                        setNumericValue(v,(cob_field*)memParams[3]);                        
                    }
                }
                // ALTER
                readLine((char*)&buf,childfd);
                v = atoi(buf);
                if (memParams[4] != NULL) {
                    if (((cob_field*)memParams[4])->data != NULL) {
                        setNumericValue(v,(cob_field*)memParams[4]);                        
                    }
                }
                readLine((char*)&buf,childfd);
                resp = atoi(buf);
                readLine((char*)&buf,childfd);
                resp2 = atoi(buf);
                if (resp > 0) {
                  abend(resp,resp2);
                }
            }

            // SET EIBRESP and EIBRESP2
            cob_put_u64_compx(resp,&eibbuf[76],4);
            cob_put_u64_compx(resp2,&eibbuf[80],4);

            if ((*respFieldsState) == 1) {
              setNumericValue(resp,(cob_field*)respFields[0]);
            }
            if ((*respFieldsState) == 2) {
              setNumericValue(resp,(cob_field*)respFields[0]);
              setNumericValue(resp,(cob_field*)respFields[1]);
            }
            (*cmdState) = 0;
            (*respFieldsState) = 0;
            return 1;
        }
        if ((var == NULL) || strstr(cmd,"'") || strstr(cmd,"MAP") || strstr(cmd,"MAPSET") || strstr(cmd,"DATAONLY") ||
            strstr(cmd,"ERASE") || strstr(cmd,"MAPONLY") || strstr(cmd,"RETURN") || strstr(cmd,"FROM") ||
            strstr(cmd,"INTO") || strstr(cmd,"HANDLE") || strstr(cmd,"CONDITION") || strstr(cmd,"ERROR") ||
            strstr(cmd,"SET") || strstr(cmd,"MAPFAIL") || strstr(cmd,"NOTFND") || strstr(cmd,"ASSIGN") ||
            strstr(cmd,"SYSID") || strstr(cmd,"TRANSID") || strstr(cmd,"COMMAREA") || strstr(cmd,"LENGTH") ||
            strstr(cmd,"CONTROL") || strstr(cmd,"FREEKB") || strstr(cmd,"PROGRAM") || strstr(cmd,"XCTL") ||
            strstr(cmd,"ABEND") || strstr(cmd,"ABCODE") || strstr(cmd,"NODUMP") || strstr(cmd,"LINK") ||
            strstr(cmd,"FLENGTH") || strstr(cmd,"DATA") || strstr(cmd,"DATAPOINTER") || strstr(cmd,"SHARED") ||
            strstr(cmd,"CWA") || strstr(cmd,"TWA") || (strstr(cmd,"EIB") && !strstr(cmd,"EIBAID")) || strstr(cmd,"TCTUA") || strstr(cmd,"TCTUALENG") || strstr(cmd,"PUT") || strstr(cmd,"GET") ||
            strstr(cmd,"CONTAINER") || strstr(cmd,"CHANNEL") || strstr(cmd,"BYTEOFFSET") || strstr(cmd,"NODATA-FLENGTH") ||
            strstr(cmd,"INTOCCSID") || strstr(cmd,"INTOCODEPAGE") || strstr(cmd,"CONVERTST") || strstr(cmd,"CCSID") ||
            strstr(cmd,"FROMCCSID") || strstr(cmd,"FROMCODEPAGE") || strstr(cmd,"DATATYPE") ||
            strstr(cmd,"APPEND") || strstr(cmd,"BIT") || strstr(cmd,"CHAR") || strstr(cmd,"CANCEL") ||
            strstr(cmd,"RESP") || strstr(cmd,"RESP2") || strstr(cmd,"RESOURCE") || strstr(cmd,"UOW") ||
            strstr(cmd,"TASK") || strstr(cmd,"NOSUSPEND") || strstr(cmd,"INITIMG") ||
            strstr(cmd,"USERDATAKEY") || strstr(cmd,"CICSDATAKEY") || strstr(cmd,"MAXLIFETIME") ||
            strstr(cmd,"ROLLBACK") || strstr(cmd,"ITEM") || strstr(cmd,"QUEUE") || strstr(cmd,"SYSID") ||
            strstr(cmd,"TS") || strstr(cmd,"TD") || strstr(cmd,"REWRITE") || strstr(cmd,"NEXT") ||
            strstr(cmd,"QNAME") || strstr(cmd,"MAIN") || strstr(cmd,"AUXILIARY") || strstr(cmd,"ABSTIME") ||
            strstr(cmd,"YYMMDD") || strstr(cmd,"YEAR") || strstr(cmd,"TIME") || strstr(cmd,"DDMMYY") ||
            strstr(cmd,"DATESEP") || strstr(cmd,"TIMESEP") || strstr(cmd,"DB2CONN") || strstr(cmd,"CONNECTST") ||
            strstr(cmd,"TRANSID") || strstr(cmd,"REQID") || strstr(cmd,"INTERVAL") || strstr(cmd,"USERID") || 
            strstr(cmd,"NOHANDLE") || strstr(cmd,"CREATE") || strstr(cmd,"CLIENT") || strstr(cmd,"SERVER") || 
            strstr(cmd,"SENDER") || strstr(cmd,"RECEIVER") || strstr(cmd,"FAULTCODE") || strstr(cmd,"FAULTCODESTR") || 
            strstr(cmd,"FAULTCODELEN") || strstr(cmd,"FAULTSTRING") || strstr(cmd,"FAULTSTRLEN") || strstr(cmd,"NATLANG") || 
            strstr(cmd,"FAULTCODE") || strstr(cmd,"ROLE") || strstr(cmd,"ROLELENGTH") || strstr(cmd,"FAULTACTOR") || 
            strstr(cmd,"FAULTACTLEN") || strstr(cmd,"DETAIL") || strstr(cmd,"DETAILLENGTH")|| strstr(cmd,"FROMCCSID") ||
            strstr(cmd,"SERVICE") || strstr(cmd,"WEBSERVICE") || strstr(cmd,"OPERATION") || strstr(cmd,"URI") ||  
            strstr(cmd,"URIMAP") || strstr(cmd,"SCOPE") || strstr(cmd,"SCOPELEN") || strstr(cmd,"NODATA") ||
            strstr(cmd,"SECURITY") || strstr(cmd,"RESTYPE") || strstr(cmd,"RESCLASS") || strstr(cmd,"RESIDLENGTH") || 
            strstr(cmd,"RESID") || strstr(cmd,"LOGMESSAGE") || strstr(cmd,"READ") || strstr(cmd,"UPDATE") ||
            strstr(cmd,"UPDATE") || strstr(cmd,"ALTER")) {
            sprintf(end,"%s%s",cmd,"\n");

            if ((strcmp(cmd,"NOHANDLE") == 0) && ((*respFieldsState) == 0)) {
                (*respFieldsState) = 3;
            }
            if (var != NULL) {
              cob_field *cobvar = (cob_field*)var;
              if (strcmp(cmd,"RESP") == 0) {
                cob_put_u64_compx(0,cobvar->data,4);
                respFields[0] = (void*)cobvar;
                (*respFieldsState) = 1;
              }
              if (strcmp(cmd,"RESP2") == 0) {
                cob_put_u64_compx(0,cobvar->data,4);
                respFields[1] = (void*)cobvar;
                (*respFieldsState) = 2;
              }
            }
            if (((*cmdState) == 2) && ((*memParamsState) == 1)) {
                // RECEIVE INTO LENGTH param value
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -2) {
                if (strcmp(cmd,"LENGTH") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"INTO") == 0) {
                    (*memParamsState) = 2;
                }
            }
            if (((*cmdState) == -3) && ((*xctlState) == 1)) {
                // XCTL PROGRAM param value
                char *progname = (cmd+1);
                int l = strlen(progname);
                if (l > 9) l = 9;
                int i = l-1;
                while ((i > 0) &&
                       ((progname[i]==' ') || (progname[i]=='\'') ||
                        (progname[i]==10) || (progname[i]==13))) {
                    i--;
                }
                l = i+1;
                if (l > 8) l = 8;
                for (i = 0; i < l; i++) {
                  xctlParams[0][i] = progname[i];
                }
                xctlParams[0][l] = 0x00;
                (*xctlState) = 10;
            }
            if ((*cmdState) == -3) {
                if (strstr(cmd,"PROGRAM")) {
                    (*xctlState) = 1;
                }
            }
            if ((*cmdState) == -4) {
                if (strstr(cmd,"INTO")) {
                    (*retrieveState) = 1;
                }
                if (strstr(cmd,"SET")) {
                    (*retrieveState) = 2;
                }
                if (strstr(cmd,"LENGTH")) {
                    (*retrieveState) = 3;
                }
            }
            if (((*cmdState) == -5) && ((*xctlState) == 1)) {
                // LINK PROGRAM param value
                char *progname = (cmd+1);
                int l = strlen(progname);
                if (l > 9) l = 9;
                int i = l-1;
                while ((i > 0) &&
                       ((progname[i]==' ') || (progname[i]=='\'') ||
                        (progname[i]==10) || (progname[i]==13))) {
                    i--;
                }
                l = i+1;
                if (l > 8) l = 8;
                for (i = 0; i < l; i++) {
                  xctlParams[0][i] = progname[i];
                }
                xctlParams[0][l] = 0x00;
                (*xctlState) = 10;
            }
            if ((*cmdState) == -5) {
              if (strstr(cmd,"PROGRAM")) {
                  (*xctlState) = 1;
              }
              if (strstr(cmd,"COMMAREA")) {
                  (*xctlState) = 2;
              }
            }
            if (((*cmdState) == -6) && ((*memParamsState) == 3)) {
                // GETMAIN INITIMG param value
                char *imgchar = (cmd+1);
                int l = strlen(imgchar);
                if (l > 2) l = 2;
                int i = l-1;
                while ((i > 0) &&
                       ((imgchar[i]==' ') || (imgchar[i]=='\'') ||
                        (imgchar[i]==10) || (imgchar[i]==13))) {
                    i--;
                }
                l = i+1;
                if (l > 1) l = 1;
                memParams[3] = (void*)&paramsBuf[3];
                for (i = 0; i < l; i++) {
                  ((char*)memParams[3])[i] = imgchar[i];
                }
                ((char*)memParams[3])[l] = 0x00;
                (*memParamsState) = 10;
            }
            if (((*cmdState) == -6) && ((*memParamsState) == 2)) {
                // GETMAIN LENGTH/FLENGTH param value
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -6) {
                if (strcmp(cmd,"SET") == 0) {
                    (*memParamsState) = 1;
                }
                if (strstr(cmd,"LENGTH")) {
                    (*memParamsState) = 2;
                }
                if (strstr(cmd,"INITIMG")) {
                    (*memParamsState) = 3;
                }
                if (strstr(cmd,"SHARED")) {
                    memParams[2] = (void*)1;
                }
            }
            if ((*cmdState) == -7) {
                if (strcmp(cmd,"DATA") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"DATAPOINTER") == 0) {
                    (*memParamsState) = 2;
                }
            }
            if ((*cmdState) == -8) {
                if (strcmp(cmd,"CWA") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"TWA") == 0) {
                    (*memParamsState) = 2;
                }
                if (strcmp(cmd,"TCTUA") == 0) {
                    (*memParamsState) = 3;
                }
                if (strcmp(cmd,"TCTUALENG") == 0) {
                    (*memParamsState) = 4;
                }
                if (strcmp(cmd,"COMMAREA") == 0) {
                    (*memParamsState) = 5;
                }
                if (strcmp(cmd,"EIB") == 0) {
                    (*memParamsState) = 6;
                }
            }
            if (((*cmdState) == -9) && ((*memParamsState) == 1)) {
                // PUT FLENGTH param value
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -9) {
                if (strcmp(cmd,"FLENGTH") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"FROM") == 0) {
                    (*memParamsState) = 2;
                }
            }
            if (((*cmdState) == -10) && ((*memParamsState) == 1)) {
                // GET FLENGTH param value
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -10) {
                if (strcmp(cmd,"FLENGTH") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"INTO") == 0) {
                    (*memParamsState) = 2;
                }
                if (strcmp(cmd,"SET") == 0) {
                    (*memParamsState) = 3;
                }
                if (strcmp(cmd,"NODATA") == 0) {
                    memParams[4] = (void*)1;
                    (*memParamsState) = 10;
                }
            }
            if (((*cmdState) == -11) && ((*memParamsState) == 1)) {
                // ENQ RESOURCE
                char *resname = (cmd+1);
                int l = strlen(resname);
                if (l > 256) l = 256;
                int i = l-1;
                while ((i > 0) &&
                       ((resname[i]==' ') || (resname[i]=='\'') ||
                        (resname[i]==10) || (resname[i]==13))) {
                    i--;
                }
                l = i+1;
                if (l > 255) l = 255;
                memParams[1] = (void*)&paramsBuf[1];
                for (i = 0; i < l; i++) {
                  ((char*)memParams[1])[i] = resname[i];
                }
                ((char*)memParams[1])[l] = 0x00;
                (*memParamsState) = 10;
            }
            if (((*cmdState) == -11) && ((*memParamsState) == 2)) {
                // ENQ LENGTH
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -11) {
                // ENQ
                if (strcmp(cmd,"RESOURCE") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"LENGTH") == 0) {
                    (*memParamsState) = 2;
                }
                if (strcmp(cmd,"NOSUSPEND") == 0) {
                    memParams[2] = (void*)1;
                }
                if (strcmp(cmd,"UOW") == 0) {
                    memParams[3] = (void*)1;
                }
                if (strcmp(cmd,"TASK") == 0) {
                    memParams[4] = (void*)1;
                }
            }
            if (((*cmdState) == -12) && ((*memParamsState) == 1)) {
                // DEQ RESOURCE
                char *resname = (cmd+1);
                int l = strlen(resname);
                if (l > 256) l = 256;
                int i = l-1;
                while ((i > 0) &&
                       ((resname[i]==' ') || (resname[i]=='\'') ||
                        (resname[i]==10) || (resname[i]==13))) {
                    i--;
                }
                l = i+1;
                if (l > 255) l = 255;
                memParams[1] = (void*)&paramsBuf[1];
                for (i = 0; i < l; i++) {
                  ((char*)memParams[1])[i] = resname[i];
                }
                ((char*)memParams[1])[l] = 0x00;
                (*memParamsState) = 10;
            }
            if (((*cmdState) == -12) && ((*memParamsState) == 2)) {
                // DEQ LENGTH
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -12) {
                // DEQ
                if (strcmp(cmd,"RESOURCE") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"LENGTH") == 0) {
                    (*memParamsState) = 2;
                }
                if (strcmp(cmd,"NOSUSPEND") == 0) {
                    memParams[2] = (void*)1;
                }
                if (strcmp(cmd,"UOW") == 0) {
                    memParams[3] = (void*)1;
                }
                if (strcmp(cmd,"TASK") == 0) {
                    memParams[4] = (void*)1;
                }
            }
            if ((*cmdState) == -13) {
                if (strcmp(cmd,"ROLLBACK") == 0) {
                    (*memParamsState) = 1;
                }
            }
            if (((*cmdState) == -14) && ((*memParamsState) == 1)) {
                // WRIEQ LENGTH param value
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -14) {
                if (strcmp(cmd,"LENGTH") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"FROM") == 0) {
                    (*memParamsState) = 2;
                }
                if ((strcmp(cmd,"QUEUE") == 0) || (strcmp(cmd,"QNAME") == 0)) {
                    (*memParamsState) = 3;
                }
                if (strcmp(cmd,"ITEM") == 0) {
                    (*memParamsState) = 4;
                }
                if (strcmp(cmd,"TD") == 0) {
                    memParams[5] = (void*)((long)memParams[5] + 1);
                }
                if (strcmp(cmd,"REWRITE") == 0) {
                    memParams[5] = (void*)((long)memParams[5] + 2);
                }
            }
            if (((*cmdState) == -15) && ((*memParamsState) == 1)) {
                // READQ LENGTH param value
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -15) {
                if (strcmp(cmd,"LENGTH") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"INTO") == 0) {
                    (*memParamsState) = 2;
                }
                if ((strcmp(cmd,"QUEUE") == 0) || (strcmp(cmd,"QNAME") == 0)) {
                    (*memParamsState) = 3;
                }
                if (strcmp(cmd,"ITEM") == 0) {
                    (*memParamsState) = 4;
                }
                if (strcmp(cmd,"TD") == 0) {
                    memParams[5] = (void*)((long)memParams[5] + 1);
                }
                if (strcmp(cmd,"NEXT") == 0) {
                    memParams[5] = (void*)((long)memParams[5] + 2);
                }
            }
            if ((*cmdState) == -16) {
                if ((strcmp(cmd,"QUEUE") == 0) || (strcmp(cmd,"QNAME") == 0)) {
                    (*memParamsState) = 3;
                }
                if (strcmp(cmd,"TD") == 0) {
                    memParams[5] = (void*)((long)memParams[5] + 1);
                }
            }
            if (((*cmdState) == -18) && ((*memParamsState) == 1)) {
                (*memParamsState) = 0;
            }
            if ((*cmdState) == -18) {
                if (strcmp(cmd,"DATESEP") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"TIMESEP") == 0) {
                    (*memParamsState) = 1;
                }
            }
            if (((*cmdState) == -19) && ((*memParamsState) == 1)) {
                // START TRANSID LENGTH param value
                (*((int*)memParams[0])) = atoi(cmd);
                (*memParamsState) = 10;
            }
            if ((*cmdState) == -19) {
                (*memParamsState) = 10;

                if (strcmp(cmd,"LENGTH") == 0) {
                  (*memParamsState) = 1;
                }
                if (strcmp(cmd,"FROM") == 0) {
                  (*memParamsState) = 2;
                }
                if (strcmp(cmd,"REQID") == 0) {
                  (*memParamsState) = 3;
                }
            }
            if ((*cmdState) == -21) {
                (*memParamsState) = 10;

                if (strcmp(cmd,"CREATE") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"CLIENT") == 0) {
                    (*memParamsState) = 2;
                }
                if (strcmp(cmd,"SERVER") == 0) {
                    (*memParamsState) = 2;
                }
                if (strcmp(cmd,"SENDER") == 0) {
                    (*memParamsState) = 2;
                }
                if (strcmp(cmd,"RECEIVER") == 0) {
                    (*memParamsState) = 2;
                }
            }
            if ((*cmdState) == -23) {
                (*memParamsState) = 10;

                if (strcmp(cmd,"READ") == 0) {
                    (*memParamsState) = 1;
                }
                if (strcmp(cmd,"UPDATE") == 0) {
                    (*memParamsState) = 2;
                }
                if (strcmp(cmd,"CONTROL") == 0) {
                    (*memParamsState) = 3;
                }
                if (strcmp(cmd,"ALTER") == 0) {
                    (*memParamsState) = 4;
                }
            }

            if (cmdbuf[0] == '\'') {
              // String constant
              write(childfd,"=",1);
            }
            write(childfd,cmdbuf,strlen(cmdbuf));
            cmdbuf[0] = 0x00;
            if ((*cmdState) == -1) {
                if (strstr(cmd,"MAP=")) {
                    sprintf(currentMap,"%s",(cmd+4));
                }
                if (strstr(cmd,"MAPSET=")) {
                    writeJson(currentMap,(cmd+7),childfd);
                }
            }
        } else {
            if (var != NULL) {
                cob_field *cobvar = (cob_field*)var;
                if ((*cmdState) == -1) {
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) putc('\'',f);
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) putc('\'',f);
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                }
                if (((*cmdState) == -2) && ((*memParamsState) == 0)) {
                    sprintf(end,"%s%s",cmd,"\n");
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    // Read in client response value
                    char buf[2048];
                    buf[0] = 0x00;
                    char c = 0x00;
                    int pos = 0;
                    while (c != '\n') {
                        int n = read(childfd,&c,1);
                        if ((n == 1) && (pos < 2047) && (c != '\n') && (c != '\r') && (c != '\'')) {
                            buf[pos] = c;
                            pos++;
                        }
                    }
                    buf[pos] = 0x00;
                    // printf("read %s\n",buf);
                    cob_put_picx(cobvar->data,cobvar->size,buf);
                }
                if (((*cmdState) == -2) && ((*memParamsState) == 2)) {
                    memParams[1] = (void*)cobvar;
                    (*memParamsState) = 10;
                    char str[20];
                    sprintf((char*)&str,"%s\n","SIZE");
                    write(childfd,str,strlen(str));
                    sprintf((char*)&str,"%s%d\n","=",(int)cobvar->size);
                    write(childfd,str,strlen(str));
                }
                if (((*cmdState) == -2) && ((*memParamsState) == 1)) {
                    // WRITEQ LENGTH
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    (*memParamsState) = 10;
                }
                if ((*cmdState) == -3) {
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) putc('\'',f);
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) putc('\'',f);
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    if ((*xctlState) == 1) {
                        // XCTL PROGRAM param value
                        char *progname = (cmdbuf+2);
                        int l = strlen(progname);
                        if (l > 9) l = 9;
                        int i = l-1;
                        while ((i > 0) &&
                               ((progname[i]==' ') || (progname[i]=='\'') ||
                                (progname[i]==10) || (progname[i]==13)))
                            i--;
                        progname[i+1] = 0x00;
                        sprintf(xctlParams[0],"%s",progname);
                        (*xctlState) = 10;
                    }
                }
                if ((*cmdState) == -4) {
                    if ((*retrieveState) == 1) {
                      // INTO
                      sprintf(end,"%d",(int)cobvar->size);
                      write(childfd,cmdbuf,strlen(cmdbuf));
                      write(childfd,"\n",1);
                      int i = 0;
                      char c;
                      for (i = 0; i < (size_t)cobvar->size; ) {
                          int n = read(childfd,&c,1);
                          if (n == 1) {
                            cobvar->data[i] = c;
                            i++;
                          }
                      }
                    }
                }
                if ((*cmdState) == -5) {
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) putc('\'',f);
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) putc('\'',f);
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    if ((*xctlState) == 1) {
                        // LINK PROGRAM param value
                        char *progname = (cmdbuf+2);
                        int l = strlen(progname);
                        if (l > 9) l = 9;
                        int i = l-1;
                        while ((i > 0) &&
                               ((progname[i]==' ') || (progname[i]=='\'') ||
                                (progname[i]==10) || (progname[i]==13)))
                            i--;
                        progname[i+1] = 0x00;
                        sprintf(xctlParams[0],"%s",progname);
                        (*xctlState) = 10;
                    }
                }
                if (((*cmdState) == -5) && ((*xctlState) == 2)) {
                    xctlParams[1] = (char*)cobvar;
                    if (((int)cobvar->size >= 0) && (cobvar->size < 32768)) {
                        for (int i = 0; i < cobvar->size; i++) {
                          commArea[i] = cobvar->data[i];
                        }
                    }
                    (*xctlState) = 10;
                }
                if (((*cmdState) < -5) &&
                    !(((*cmdState) == -9) && ((*memParamsState) == 1)) &&
                    !(((*cmdState) == -9) && ((*memParamsState) == 2)) &&
                    !(((*cmdState) == -10) && ((*memParamsState) == 1)) &&
                    !(((*cmdState) == -10) && ((*memParamsState) == 2)) &&
                    !(((*cmdState) == -10) && ((*memParamsState) == 3)) &&
                    !(((*cmdState) == -6) && ((*memParamsState) == 2))  &&
                    !(((*cmdState) == -11) && ((*memParamsState) == 2)) &&
                    !(((*cmdState) == -12) && ((*memParamsState) == 2)) &&
                    !(((*cmdState) == -14) && ((*memParamsState) == 0)) &&
                    !(((*cmdState) == -14) && ((*memParamsState) == 1)) &&
                    !(((*cmdState) == -15) && ((*memParamsState) == 0)) &&
                    !(((*cmdState) == -15) && ((*memParamsState) == 1)) &&
                    !(((*cmdState) == -15) && ((*memParamsState) == 2)) &&
                    !(((*cmdState) == -18) && ((*memParamsState) == 0)) &&
                    !(((*cmdState) == -18) && ((*memParamsState) == 1)) &&
                    !(((*cmdState) == -19) && ((*memParamsState) == 1)) &&
                    !(((*cmdState) == -19) && ((*memParamsState) == 2)) &&
                    !(((*cmdState) == -19) && ((*memParamsState) == 3)) &&
                    !(((*cmdState) == -21) && ((*memParamsState) == 1)) &&
                    !(((*cmdState) == -21) && ((*memParamsState) == 2)) &&
                    !(((*cmdState) == -23) && ((*memParamsState) == 1)) &&
                    !(((*cmdState) == -23) && ((*memParamsState) == 2)) &&
                    !(((*cmdState) == -23) && ((*memParamsState) == 3)) &&
                    !(((*cmdState) == -23) && ((*memParamsState) == 4))) {
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) putc('\'',f);
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) putc('\'',f);
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                }
                if (((*cmdState) == -6) && ((*memParamsState) == 1)) {
                  memParams[1] = (void*)cobvar;
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -6) && ((*memParamsState) == 2)) {
                    // GETMAIN LENGTH/FLENGTH param value
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -6) && ((*memParamsState) == 3)) {
                  // GETMAIN INITIMG param value
                  if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) {
                    memParams[3] = (void*)&paramsBuf[3];
                    ((char*)memParams[3])[0] = cobvar->data[0];
                    ((char*)memParams[3])[1] = 0x00;
                    (*memParamsState) = 10;
                  }
                }
                if (((*cmdState) == -7) && ((*memParamsState) == 1)) {
                  memParams[1] = (void*)cobvar->data;
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -7) && ((*memParamsState) == 2)) {
                  memParams[1] = (void*)(*((unsigned char**)cobvar->data));
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -8) && ((*memParamsState) == 1)) {
                  (*((unsigned char**)cobvar->data)) = cwa;
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -8) && ((*memParamsState) == 2)) {
                  (*((unsigned char**)cobvar->data)) = (unsigned char*)twa;
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -8) && ((*memParamsState) == 3)) {
                  (*((unsigned char**)cobvar->data)) = (unsigned char*)tua;
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -8) && ((*memParamsState) == 4)) {
                  (*((unsigned char**)cobvar->data)) = (unsigned char*)tua;
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -8) && ((*memParamsState) == 5)) {
                  (*((unsigned char**)cobvar->data)) = (unsigned char*)commArea;
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -8) && ((*memParamsState) == 6)) {
                  (*((unsigned char**)cobvar->data)) = (unsigned char*)eibbuf;
                  (*memParamsState) = 10;
                }
                if (((*cmdState) == -9) && ((*memParamsState) == 1)) {
                    // PUT FLENGTH param value
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -9) && ((*memParamsState) == 2)) {
                  memParams[1] = (void*)cobvar;
                  (*memParamsState) = 10;
                  char str[20];
                  sprintf((char*)&str,"%s\n","SIZE");
                  write(childfd,str,strlen(str));
                  sprintf((char*)&str,"%s%d\n","=",(int)cobvar->size);
                  write(childfd,str,strlen(str));
                }
                if (((*cmdState) == -10) && ((*memParamsState) == 1)) {
                    // GET FLENGTH param value
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    memParams[3] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -10) && ((*memParamsState) == 2)) {
                    memParams[1] = (void*)cobvar;
                    (*memParamsState) = 10;
                    char str[20];
                    sprintf((char*)&str,"%s\n","SIZE");
                    write(childfd,str,strlen(str));
                    sprintf((char*)&str,"%s%d\n","=",(int)cobvar->size);
                    write(childfd,str,strlen(str));
                }
                if (((*cmdState) == -10) && ((*memParamsState) == 3)) {
                    memParams[2] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -11) && ((*memParamsState) == 1)) {
                    // ENQ RESOURCE
                    memParams[1] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -11) && ((*memParamsState) == 2)) {
                    // ENQ LENGTH
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -12) && ((*memParamsState) == 1)) {
                    // DEQ RESOURCE
                    memParams[1] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -12) && ((*memParamsState) == 2)) {
                    // DEQ LENGTH
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -14) && ((*memParamsState) == 4)) {
                    memParams[3] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -14) && ((*memParamsState) == 2)) {
                    memParams[1] = (void*)cobvar;
                    (*memParamsState) = 10;
                    char str[20];
                    sprintf((char*)&str,"%s\n","SIZE");
                    write(childfd,str,strlen(str));
                    sprintf((char*)&str,"%s%d\n","=",(int)cobvar->size);
                    write(childfd,str,strlen(str));
                }
                if (((*cmdState) == -14) && ((*memParamsState) == 1)) {
                    // WRITEQ LENGTH
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -15) && ((*memParamsState) == 4)) {
                    memParams[3] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -15) && ((*memParamsState) == 2)) {
                    memParams[1] = (void*)cobvar;
                    (*memParamsState) = 10;
                    char str[20];
                    sprintf((char*)&str,"%s\n","SIZE");
                    write(childfd,str,strlen(str));
                    sprintf((char*)&str,"%s%d\n","=",(int)cobvar->size);
                    write(childfd,str,strlen(str));
                }
                if (((*cmdState) == -15) && ((*memParamsState) == 1)) {
                    // READQ LENGTH
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -18) && ((*memParamsState) == 0)) {
                    // General Read-Only data handling
                    char buf[2048];
                    readLine((char*)&buf,childfd);

                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) {
                        for (int i = 0; i < cobvar->size; i++) {
                          cobvar->data[i] = buf[i];
                        }
                    }
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_NUMERIC) {
                        char hbuf[256];
                        cob_put_picx(cobvar->data,cobvar->size,
                            convertNumeric(buf,cobvar->attr->digits,
                                           cobvar->attr->scale,hbuf));
                    }
                    if (COB_FIELD_TYPE(cobvar) == COB_TYPE_NUMERIC_PACKED) {
                      long v = atol(buf);
                      cob_put_s64_comp3(v,cobvar->data,cobvar->size);
                    }
                    if (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) {
                      long v = atol(buf);
                      cob_put_u64_compx(v,cobvar->data,cobvar->size);
                    }
                    if (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) {
                      long v = atol(buf);
                      cob_put_s64_comp5(v,cobvar->data,cobvar->size);
                    }
                }
                if (((*cmdState) == -18) && ((*memParamsState) == 1)) {
                    (*memParamsState) = 0;
                }
                if (((*cmdState) == -19) && ((*memParamsState) == 3)) {
                    // START TRANSID REQID
                    write(childfd,"=",1);
                    write(childfd,"'",1);
                    write(childfd,cobvar->data,8);
                    write(childfd,"'\n",2);
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -19) && ((*memParamsState) == 2)) {
                    memParams[1] = (void*)cobvar;
                    (*memParamsState) = 10;
                    char str[20];
                    sprintf((char*)&str,"%s\n","SIZE");
                    write(childfd,str,strlen(str));
                    sprintf((char*)&str,"%s%d\n","=",(int)cobvar->size);
                    write(childfd,str,strlen(str));
                }
                if (((*cmdState) == -19) && ((*memParamsState) == 1)) {
                    // START TRANSID LENGTH
                    sprintf(end,"%s%s",cmd,"=");
                    end = &cmdbuf[strlen(cmdbuf)];
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((cobvar->data[0] != 0) || (getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                        display_cobfield(cobvar,f);
                    }
                    putc(0x00,f);
                    fclose(f);
                    write(childfd,cmdbuf,strlen(cmdbuf));
                    write(childfd,"\n",1);
                    (*((int*)memParams[0])) = atoi(end);
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -21) && ((*memParamsState) == 1)) {
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -21) && ((*memParamsState) == 2)) {
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -23) && ((*memParamsState) == 1)) {
                    memParams[1] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -23) && ((*memParamsState) == 2)) {
                    memParams[2] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -23) && ((*memParamsState) == 3)) {
                    memParams[3] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
                if (((*cmdState) == -23) && ((*memParamsState) == 4)) {
                    memParams[4] = (void*)cobvar;
                    (*memParamsState) = 10;
                }
            }
            cmdbuf[0] = 0x00;
        }
        return 1;
    }
    if (strstr(cmd,"END-EXEC")) {
        cmdbuf[strlen(cmdbuf)-1] = '\n';
        cmdbuf[strlen(cmdbuf)] = 0x00;
//      write(childfd,cmdbuf,strlen(cmdbuf));
        cmdbuf[strlen(cmdbuf)-1] = 0x00;
        processCmd(cmdbuf,outputVars);
        cmdbuf[0] = 0x00;
        (*cmdState) = 0;
        outputVars[0] = NULL; // NULL terminated list
    } else {
        if ((strlen(cmd) == 0) && (var != NULL)) {
            cob_field *cobvar = (cob_field*)var;
            if ((*cmdState) < 2) {
                if (COB_FIELD_TYPE(cobvar) == COB_TYPE_GROUP) {
		    // Treat as VARCHAR field
		    unsigned int l = (unsigned int)cobvar->data[0];	
                    l = (l << 8) | (unsigned int)cobvar->data[1];
		            if (l > (cobvar->size-2)) {
                       l = cobvar->size-2;
                    }
                    end[0] = '\'';
                    int i = 0, j = 1;
                    for (i = 0; i < l; i++, j++) {
                        unsigned char c = cobvar->data[i+2];
                        if (c == 0x00) {
                           end[j] = '\\';
                           j++;
                           end[j] = '0';  
                           continue; 
                        }   
                        if ((c & 0x80) == 0) {
                           // Plain ASCII
                           end[j] = c; 
                        } else {
                           // Convert ext. ASCII to UTF-8
                           unsigned char c1 = 0xC0;
                           c1 = c1 | ((c & 0xC0) >> 6);
                           end[j] = c1; 
                           j++;
                           c1 = 0x80;
                           c1 = c1 | (c & 0x3F);
                           end[j] = c1;
                        }
                    }
                    end[j] = '\'';
                    end[j+1] = ' ';
                    end[j+2] = 0x00;
                } else
                if (COB_FIELD_TYPE(cobvar) == COB_TYPE_ALPHANUMERIC) {
                    char *str = adjustDateFormatToDb((char*)cobvar->data,cobvar->size);
                    end[0] = '\'';
                    int i = 0, j = 1;
                    for (i = 0; i < cobvar->size; i++, j++) {
                        unsigned char c = str[i];
                        if (c == 0x00) {
                           end[j] = '\\';
                           j++;
                           end[j] = '0';  
                           continue; 
                        }
                        if ((c & 0x80) == 0) {
                           // Plain ASCII
                           end[j] = c; 
                        } else {
                           // Convert ext. ASCII to UTF-8
                           unsigned char c1 = 0xC0;
                           c1 = c1 | ((c & 0xC0) >> 6);
                           end[j] = c1; 
                           j++;
                           c1 = 0x80;
                           c1 = c1 | (c & 0x3F);
                           end[j] = c1;
                        }
                    }
                    end[j] = '\'';
                    end[j+1] = ' ';
                    end[j+2] = 0x00;
                } else {
                    FILE *f = fmemopen(end, CMDBUF_SIZE-strlen(cmdbuf), "w");
                    if ((getCobType(cobvar) == COB_TYPE_NUMERIC_BINARY) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_COMP5) ||
                        (getCobType(cobvar) == COB_TYPE_NUMERIC) || 
                        (getCobType(cobvar) == COB_TYPE_NUMERIC_PACKED)) {
                       display_cobfield(cobvar,f);
                    }
                    putc(' ',f);
                    putc(0x00,f);
                    fclose(f);
                }
            } else {
                int index = (*cmdState)-2;
                if (index <= 98) {
                    outputVars[index] = cobvar;
                    outputVars[index+1] = NULL;
                }
                (*cmdState)++;
            }
        } else {
            if (strstr(cmd,"SELECT") || strstr(cmd,"FETCH")) {
                (*cmdState) = 1;
            } else {
                if (strstr(cmd,"INTO") && ((*cmdState) == 1)) {
                    (*cmdState) = 2;
                } else {
                    if ((strstr(cmd,",") == NULL) && (*cmdState) >= 2) {
                        (*cmdState) = 0;
                    }
                }
            }
            if ((*cmdState) < 2) {
                sprintf(end,"%s%s",cmd," ");
            }
        }
    }
    return 1;
}


static void segv_handler(int signo)
{
    if (signo == SIGSEGV) {
        printf("Segmentation fault in QWICS tpmserver, abending task\n");
        int *runState = (int*)pthread_getspecific(runStateKey);
        (*runState) = 3;
        int *respFieldsState = (int*)pthread_getspecific(respFieldsStateKey);
        (*respFieldsState) = 0;
        abend(16,1);
    }
    exit(0);
}


// Manage load module executor
void initExec(int initCons) {
    performEXEC = &execCallback;
    resolveCALL = &callCallback;
    cobinit();
    pthread_key_create(&childfdKey, NULL);
    pthread_key_create(&connKey, NULL);
    pthread_key_create(&cmdbufKey, NULL);
    pthread_key_create(&cmdStateKey, NULL);
    pthread_key_create(&runStateKey, NULL);
    pthread_key_create(&cobFieldKey, NULL);
    pthread_key_create(&xctlStateKey, NULL);
    pthread_key_create(&xctlParamsKey, NULL);
    pthread_key_create(&eibbufKey, NULL);
    pthread_key_create(&linkAreaKey, NULL);
    pthread_key_create(&linkAreaPtrKey, NULL);
    pthread_key_create(&linkAreaAdrKey, NULL);
    pthread_key_create(&commAreaKey, NULL);
    pthread_key_create(&commAreaPtrKey, NULL);
    pthread_key_create(&areaModeKey, NULL);
    pthread_key_create(&linkStackKey, NULL);
    pthread_key_create(&linkStackPtrKey, NULL);
    pthread_key_create(&memParamsKey, NULL);
    pthread_key_create(&memParamsStateKey, NULL);
    pthread_key_create(&twaKey, NULL);
    pthread_key_create(&tuaKey, NULL);
    pthread_key_create(&respFieldsStateKey, NULL);
    pthread_key_create(&respFieldsKey, NULL);
    pthread_key_create(&taskLocksKey, NULL);
    pthread_key_create(&callStackKey, NULL);
    pthread_key_create(&callStackPtrKey, NULL);
    pthread_key_create(&chnBufListKey, NULL);
    pthread_key_create(&chnBufListPtrKey, NULL);

#ifndef _USE_ONLY_PROCESSES_
    pthread_mutex_init(&moduleMutex,NULL);
    pthread_cond_init(&waitForModuleChange,NULL);
#endif

    for (int i = 0; i < 100; i++) {
        condHandler[i] = NULL;
    }
    initSharedMalloc(initCons);
    sharedAllocMem = (void**)sharedMalloc(11,MEM_POOL_SIZE*sizeof(void*));
    sharedAllocMemLen = (int*)sharedMalloc(14,MEM_POOL_SIZE*sizeof(int));
    sharedAllocMemPtr = (int*)sharedMalloc(12,sizeof(int));
    cwa = (unsigned char*)sharedMalloc(13,4096);
    initEnqResources(initCons);

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&sharedMemMutex,&attr);

    setUpPool(10, GETENV_STRING(connectStr,"QWICS_DB_CONNECTSTR","dbname=qwics"), initCons);
    currentMap[0] = 0x00;

    GETENV_STRING(cobDateFormat,"QWICS_COBDATEFORMAT","YYYY-MM-dd.hh:mm:ss.uuuu");
}


void clearExec(int initCons) {
#ifndef _USE_ONLY_PROCESSES_
    pthread_cond_destroy(&waitForModuleChange);
#endif
    tearDownPool(initCons);
    sharedFree(sharedAllocMem,MEM_POOL_SIZE*sizeof(void*));
    sharedFree(sharedAllocMemLen,MEM_POOL_SIZE*sizeof(int));
    sharedFree(sharedAllocMemPtr,sizeof(int));
    sharedFree(cwa,4096);

    for (int i = 0; i < 100; i++) {
        if (condHandler[i] != NULL) {
            free(condHandler[i]);
        }
    }
}


void execTransaction(char *name, void *fd, int setCommArea, int parCount) {
    char cmdbuf[CMDBUF_SIZE];
    int cmdState = 0;
    int runState = 0;
    int xctlState = 0;
    char progname[9];
    char *xctlParams[10];
    char eibbuf[150];
    char *linkArea = malloc(16000000);
    char commArea[32768];
    int linkAreaPtr = 0;
    char *linkAreaAdr = linkArea;
    int commAreaPtr = 0;
    int areaMode = 0;
    char linkStack[900];
    int linkStackPtr = 0;
    int memParamsState = 0;
    void *memParams[10];
    int memParam = 0;
    char twa[32768];
    char tua[256];
    void** allocMem = (void**)malloc(MEM_POOL_SIZE*sizeof(void*));
    int allocMemPtr = 0;
    int respFieldsState = 0;
    void *respFields[2];
    struct taskLock *taskLocks = createTaskLocks();
    int callStackPtr = 0;
    struct callLoadlib callStack[1024];
    int chnBufListPtr = 0;
    struct chnBuf chnBufList[256];
    int i = 0;
    for (i= 0; i < 150; i++) eibbuf[i] = 0;
    xctlParams[0] = progname;
    memParams[0] = &memParam;
    cob_field* outputVars[100];
    outputVars[0] = NULL; // NULL terminated list
    cmdbuf[0] = 0x00;
    pthread_setspecific(childfdKey, fd);
    pthread_setspecific(cmdbufKey, &cmdbuf);
    pthread_setspecific(cmdStateKey, &cmdState);
    pthread_setspecific(runStateKey, &runState);
    pthread_setspecific(cobFieldKey, &outputVars);
    pthread_setspecific(xctlStateKey, &xctlState);
    pthread_setspecific(xctlParamsKey, &xctlParams);
    pthread_setspecific(eibbufKey, &eibbuf);
    pthread_setspecific(linkAreaKey, linkArea);
    pthread_setspecific(linkAreaPtrKey, &linkAreaPtr);
    pthread_setspecific(linkAreaAdrKey, &linkAreaAdr);
    pthread_setspecific(commAreaKey, &commArea);
    pthread_setspecific(commAreaPtrKey, &commAreaPtr);
    pthread_setspecific(areaModeKey, &areaMode);
    pthread_setspecific(linkStackKey, &linkStack);
    pthread_setspecific(linkStackPtrKey, &linkStackPtr);
    pthread_setspecific(memParamsKey, &memParams);
    pthread_setspecific(memParamsStateKey, &memParamsState);
    pthread_setspecific(twaKey, &twa);
    pthread_setspecific(tuaKey, &tua);
    pthread_setspecific(allocMemKey, allocMem);
    pthread_setspecific(allocMemPtrKey, &allocMemPtr);
    pthread_setspecific(respFieldsStateKey, &respFieldsState);
    pthread_setspecific(respFieldsKey, &respFields);
    pthread_setspecific(taskLocksKey, taskLocks);
    pthread_setspecific(callStackKey, &callStack);
    pthread_setspecific(callStackPtrKey, &callStackPtr);
    pthread_setspecific(chnBufListKey, &chnBufList);
    pthread_setspecific(chnBufListPtrKey, &chnBufListPtr);

    // Optionally read in content of commarea
    if (setCommArea == 1) {
      write(*(int*)fd,"COMMAREA\n",9);
      char c = 0x00;
      for (i = 0; i < 32768; ) {
        int n = read(*(int*)fd,&c,1);
        if (n == 1) {
          commArea[i] = c;
          i++;
        }
      }
    }

    cob_get_global_ptr()->cob_current_module = &thisModule;
    cob_get_global_ptr()->cob_call_params = 1;

    // Optionally initialize call params
    if ((parCount > 0) && (parCount <= 10)) {
        cob_get_global_ptr ()->cob_call_params = cob_get_global_ptr ()->cob_call_params + parCount;
        for (i = 0; i < parCount; i++) {
            char len[10];
            char c = 0x00;
            int pos = 0;
            while (c != '\n') {
                int n = read(*(int*)fd,&c,1);
                if ((n == 1) && (c != '\n') && (c != '\r') && (c != '\'') && (pos < 10)) {
                    len[pos] = c;
                    pos++;
                }
            }
            len[pos] = 0x00; 
            paramList[i] = (void*)&linkArea[linkAreaPtr];
            linkAreaAdr = &linkArea[linkAreaPtr];
            linkAreaPtr += atoi(len);
        }
    }

    // Set signal handler for SIGSEGV (in case of mem leak in load module)
    struct sigaction a;
    a.sa_handler = segv_handler;
    a.sa_flags = 0;
    sigemptyset( &a.sa_mask );
    sigaction( SIGSEGV, &a, NULL );

    PGconn *conn = getDBConnection();
    pthread_setspecific(connKey, (void*)conn);
    initMain();
    execLoadModule(name,0,parCount);
    releaseLocks(TASK,taskLocks);
    globalCallCleanup();
    clearMain();
    free(allocMem);
    free(linkArea);
    clearChnBufList();
    returnDBConnection(conn,1);
    // Flush output buffers
    fflush(stdout);
    fflush(stderr);
}


// Exec COBOL module within an existing DB transaction
void execInTransaction(char *name, void *fd, int setCommArea, int parCount) {
    char cmdbuf[CMDBUF_SIZE];
    int cmdState = 0;
    int runState = 0;
    int xctlState = 0;
    char *xctlParams[10];
    char eibbuf[150];
    char *linkArea = malloc(16000000);
    char commArea[32768];
    int linkAreaPtr = 0;
    char *linkAreaAdr = linkArea;
    int commAreaPtr = 0;
    int areaMode = 0;
    char progname[9];
    char linkStack[900];
    int linkStackPtr = 0;
    int memParamsState = 0;
    void *memParams[10];
    int memParam = 0;
    char twa[32768];
    char tua[256];
    void** allocMem = (void**)malloc(MEM_POOL_SIZE*sizeof(void*));
    int allocMemPtr = 0;
    int respFieldsState = 0;
    void *respFields[2];
    struct taskLock *taskLocks = createTaskLocks();
    int callStackPtr = 0;
    struct callLoadlib callStack[1024];
    int chnBufListPtr = 0;
    struct chnBuf chnBufList[256];
    int i = 0;
    for (i= 0; i < 150; i++) eibbuf[i] = 0;
    xctlParams[0] = progname;
    memParams[0] = &memParam;
    cob_field* outputVars[100];
    outputVars[0] = NULL; // NULL terminated list
    cmdbuf[0] = 0x00;
    pthread_setspecific(childfdKey, fd);
    pthread_setspecific(cmdbufKey, &cmdbuf);
    pthread_setspecific(cmdStateKey, &cmdState);
    pthread_setspecific(runStateKey, &runState);
    pthread_setspecific(cobFieldKey, &outputVars);
    pthread_setspecific(xctlStateKey, &xctlState);
    pthread_setspecific(xctlParamsKey, &xctlParams);
    pthread_setspecific(eibbufKey, &eibbuf);
    pthread_setspecific(linkAreaKey, linkArea);
    pthread_setspecific(linkAreaPtrKey, &linkAreaPtr);
    pthread_setspecific(linkAreaAdrKey, &linkAreaAdr);
    pthread_setspecific(commAreaKey, &commArea);
    pthread_setspecific(commAreaPtrKey, &commAreaPtr);
    pthread_setspecific(areaModeKey, &areaMode);
    pthread_setspecific(linkStackKey, &linkStack);
    pthread_setspecific(linkStackPtrKey, &linkStackPtr);
    pthread_setspecific(memParamsKey, &memParams);
    pthread_setspecific(memParamsStateKey, &memParamsState);
    pthread_setspecific(twaKey, &twa);
    pthread_setspecific(tuaKey, &tua);
    pthread_setspecific(allocMemKey, allocMem);
    pthread_setspecific(allocMemPtrKey, &allocMemPtr);
    pthread_setspecific(respFieldsStateKey, &respFieldsState);
    pthread_setspecific(respFieldsKey, &respFields);
    pthread_setspecific(taskLocksKey, taskLocks);
    pthread_setspecific(callStackKey, &callStack);
    pthread_setspecific(callStackPtrKey, &callStackPtr);
    pthread_setspecific(chnBufListKey, &chnBufList);
    pthread_setspecific(chnBufListPtrKey, &chnBufListPtr);

    // Oprionally read in content of commarea
    if (setCommArea == 1) {
      write(*(int*)fd,"COMMAREA\n",9);
      char c = 0x00;
      for (i = 0; i < 32768; ) {
        int n = read(*(int*)fd,&c,1);
        if (n == 1) {
          commArea[i] = c;
          i++;
        }
      }
    }
    
    cob_get_global_ptr()->cob_current_module = &thisModule;
    cob_get_global_ptr()->cob_call_params = 1;

    // Optionally initialize call params
    if ((parCount > 0) && (parCount <= 10)) {
        cob_get_global_ptr ()->cob_call_params = cob_get_global_ptr ()->cob_call_params + parCount;
        for (i = 0; i < parCount; i++) {
            char len[10];
            char c = 0x00;
            int pos = 0;
            while (c != '\n') {
                int n = read(*(int*)fd,&c,1);
                if ((n == 1) && (c != '\n') && (c != '\r') && (c != '\'') && (pos < 10)) {
                    len[pos] = c;
                    pos++;
                }
            }
            len[pos] = 0x00; 
            paramList[i] = (void*)&linkArea[linkAreaPtr];
            linkAreaAdr = &linkArea[linkAreaPtr];
            linkAreaPtr += atoi(len);
        }
    }

    // Set signal handler for SIGSEGV (in case of mem leak in load module)
    struct sigaction a;
    a.sa_handler = segv_handler;
    a.sa_flags = 0;
    sigemptyset( &a.sa_mask );
    sigaction( SIGSEGV, &a, NULL );

    initMain();
    execLoadModule(name,0,parCount);
    releaseLocks(TASK,taskLocks);
    globalCallCleanup();
    clearMain();
    free(allocMem);
    free(linkArea);
    clearChnBufList();
    // Flush output buffers
    fflush(stdout);
    fflush(stderr);
}
