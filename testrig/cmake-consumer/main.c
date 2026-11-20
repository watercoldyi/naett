#include <naett.h>
#include <stdio.h>

int main(void) {
    // 能编译并链接到安装后的 naett 即可
    naettInit(NULL);
    printf("naett consumer linked\n");
    return 0;
}
