// print the string s stored at addr
void printStrAddr(unsigned addr);
// print the string s
void printStr(const char *str);
// print the char stored at addr
void printCharAddr(unsigned addr);
// print the char c
void printChar(unsigned c);

// integrate a single LC-3 assembly instruction ins (no \n)
void integrateLC3Asm(const char *ins);

// load value at label into des
unsigned loadLabel(const char *label);
// load value at addr into des
unsigned loadAddr(unsigned addr);
// read the address of label into des
unsigned readLabelAddr(const char *label);
// store value of src into label
void storeLabel(unsigned src, const char *label);
// store value of src into addr
void storeAddr(unsigned src, unsigned addr);

#ifdef DEBUG

void printInt(unsigned x) {
  if (x > 10)
    printInt(x / 10);
  printChar('0' + x % 10);
}

#define printStrIntStr(spre, x, ssuf)                                          \
  printStr(spre), printInt(x), printStr(ssuf);

#define printStrCharStr(spre, c, ssuf)                                         \
  printStr(spre), printChar(c), printStr(ssuf);

#endif