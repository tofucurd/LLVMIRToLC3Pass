// print the string s stored at addr
void printStrAddr(int addr);
// print the string s
void printStrImm(const char *str);
// print the char stored at addr
void printCharAddr(int addr);
// print the char c
void printCharImm(char c);

// integrate a single LC-3 assembly instruction ins (no \n)
void integrateLC3Asm(const char *ins);

// load value at label into des
void loadLabel(int *des, const char *label);
// load value at addr into des
void loadAddr(int *des, int addr);
// read the address of label into des
void readLabelAddr(int *des, const char *label);
// store value of src into label
void storeLabel(int src, const char *label);
// store value of src into addr
void storeAddr(int src, int addr);