#ifndef XINPUT1_3_PROVIDED_SYMBOLS_H
#define XINPUT1_3_PROVIDED_SYMBOLS_H
#include "SymbolResolver.h"

void hookRequiredSymbols(SymbolResolver& provider);

void* provideSymbolImplementation(const char* mangledName);

#endif //XINPUT1_3_PROVIDED_SYMBOLS_H
