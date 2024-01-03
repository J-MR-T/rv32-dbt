
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

int printHelloWorld(){
    // hello world
    char test[13];
    test[0] = 'H';
    test[1] = 'e';
    test[2] = 'l';
    test[3] = 'l';
    test[4] = 'o';
    test[5] = ' ';
    test[6] = 'W';
    test[7] = 'o';
    test[8] = 'r';
    test[9] = 'l';
    test[10] = 'd';
    test[11] = '\n';
    test[12] = '\0';

    write(1, test, 13);

    return 0;
}

// this is what happens when your isa doesn't have a multiply instruction, you know?
int mul(int a, int b){
    int result = 0;
    for(int aCpy = a; aCpy > 0; aCpy--){
        result += b;
    }
    return result;
}

int div(int a, int b){
    int result = -1;
    for(int aCpy = a; aCpy > 0; aCpy -= b){
		result++;
    }
    return result;
}

int mod(int a, int b){
    int division = div(a,b); // a/b
    return a - mul(division, b);
}

// different performance tests
int multest(){
    return mul(234523452, 922463) % 256;
}

void _start()
{
    printHelloWorld();
    exit(multest());

    exit(0);
}

