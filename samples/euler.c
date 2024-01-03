
// syscalls without stdlib:

// hopefully shoulnd't inline because asm() anyway, but just in case
void __attribute__ ((noinline))  write(int fd, char* str, int len){
    (void)fd, (void)str, (void)len;
    asm volatile (
        "li a7, 64;"
        "ecall;"
    );
}

int putchar(char c){
	write(1, &c, 1);
	return 0;
}

void __attribute__ ((noinline))  exit(int code){
    (void)code;
    asm volatile (
        //"li a0, 55; 
        "li a7, 93;"
        "ecall"
    );
	__builtin_unreachable();
}

int eulertest(int n) {
    int v[n*8];
    int i = 0;
    int col = 0;
    int c = 0;
    int a = 0;

    i = col = 0;
    while(i<n){
        v[i] = 1;
        i=i+1;
    }
    while(col<n << 1) {
        a = n+1;
        c = i = 0;
        while (i<n) {
            c = c + v[i]*10;
            v[i]  = c%a;
            i=i+1;
            c = c / a;
            a=a-1;
        }

        putchar(c+48); // 48=='0'
        col = col +1;
        if(!(col % 5)){
            //putchar(col%50?' ': '*n');
            if(col % 50){
                putchar(20); // ' '
            }else{
                putchar(10); // '\n'
            }

        }
    }
    putchar(10);
    putchar(10);
    return 0;
}

void _start()
{
    //printHelloWorld();
	//exit(multest());
	exit(eulertest(2000));

    exit(0);
}


