#ifndef KSTRING_H
#define KSTRING_H

int strcmp(const char *s1, const char *s2);
char *strcpy(char *dest, const char *src);
unsigned int strlen(const char *s);
char *strncpy(char *dest, const char *src, unsigned int n);
int strncmp(const char *s1, const char *s2, unsigned int n);

#endif
