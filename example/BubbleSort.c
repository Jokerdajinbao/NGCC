#include <stdio.h>

//n*(n-1)/2

void bubbleSort(int array[], int num)
{
    int i,j;
    int temp;
    for (i=0; i<num - 1; i++)
    {
        for (j=0; j<num - 1 - i; j++)
        {
            if (array[j] > array[j+1])
            {
                temp = array[j+1];
                array[j+1] = array[j];
                array[j] = temp;
            }
        }
    }
}

int main()
{
    int k;
    int num;
    int array[100] = {7,2,8,5,1,34,21,58};
    num = 8;

    bubbleSort(array, num);

    for(k=0; k<num; k++)   
       printf("%d  ",array[k]);  
    printf("\n");  
    return 0;
}