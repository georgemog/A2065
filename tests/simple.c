#include <proto/dos.h>

int main(void)
{
    BPTR fh;
    fh = Open("ram:simple.txt", MODE_NEWFILE);
    if (fh) {
        Write(fh, "hello\n", 6);
        Close(fh);
    }
    return 0;
}
