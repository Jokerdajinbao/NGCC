#include <stdio.h>
#include "multi_add.h"

int add(int a, int b)
{
    int sum = a + b;
    printf("%d + %d = %d\n", a, b, sum);
    return sum;
}
