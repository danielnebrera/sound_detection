/* Implementación de DebugLog para TFLite Micro en STM32 */
#include <stdio.h>
extern void DebugLog(const char* s);
void DebugLog(const char* s) {
    printf("%s", s);
}