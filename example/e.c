#include<stdio.h>
#include<math.h>
long jie(int i)
{
	long j=1;
	if(i==0)
		return 1;
	else
	{
		while(i!=1)
		{
			j*=i;
			i--;
		}
		return j;
	}
}
double e(double x,int n)
{
	return pow(x,n)/(double)jie(n);
}
int main()
{
	int i=0,n=1;
	double e6=0;
	while(e(n,i)>0.0000000001)
	{
		e6+=e(n,i);
		i++;
	}
	printf("%.10lf\n",e6);
	return 0;
}