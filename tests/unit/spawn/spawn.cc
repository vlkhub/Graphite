// This unit test will test the spawning mechanisms of the simulator

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include "test_support.h"

void* thread_func(void *threadid);
void* thread_func_simple(void *threadid);

void do_a_sim()
{
   const unsigned int numThreads = 5;
   int threads[numThreads];

   for(unsigned int j = 0; j < numThreads; j++)
       threads[j] = CarbonSpawnThread(thread_func, (void *) (j + 1));

   for(unsigned int j = 0; j < numThreads; j++)
       CarbonJoinThread(threads[j]);

}

int main(int argc, char **argv)
{
   int i = 0;
   CarbonStartSim();

   for(i = 0; i < 120; i++)
   {
       do_a_sim();
   }
   CarbonStopSim();

   fprintf(stderr, "done.\n");
   return 0;
}

void* thread_func(void *threadid)
{
//   int tid = CarbonSpawnThread(thread_func_simple, (void*)1);
//   CarbonJoinThread(tid);
   return 0;
}

void* thread_func_simple(void *threadid)
{
//   usleep(250);
   return 0;
}
