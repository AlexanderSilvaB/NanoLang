#ifndef CUBE_util_h
#define CUBE_util_h

#include <stdbool.h>
#include <stdint.h>

char *readFile(const char *path, bool verbose);
char *getFolder(const char* path);
int countBytes(const void* raw, int maxSize);
void replaceString(char *str, const char* find, const char* replace);
void replaceStringN(char *str, const char *find, const char *replace, int n);
char* findFile(const char* name);
bool existsFile(const char *name);
bool isValidType(const char* name);
char *getFileName(char *path);
char *getFileDisplayName(char *path);
uint64_t cube_clock();
void cube_wait(uint64_t t);

#endif