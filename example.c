#include "pinmagic.h"

int function_one (int b);
int function_two (int *b);

int main ()
{
   int i=0;
   int a=1;
   int b=1;
   PIN_ZONE_ENTER(1);
   for(i=0; i<5; i++)
   {
       b=function_two(&b);
       a=function_one(a);
   }
   PIN_ZONE_EXIT(1);
   return 1;
}

int function_one (int b)
{
   PIN_REGION(1);
   int a;
   a = 2*b;
   return a;
}

int function_two (int *b)
{
   PIN_REGION(2);
   int a;
   a = function_one (2+*b);
   return a;
}
