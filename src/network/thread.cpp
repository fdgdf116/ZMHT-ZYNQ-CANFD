
#include "net_common.h"
#include "thread.h"
#include <signal.h>
#include <time.h>

Mutex::Mutex(bool init/* = false*/)
	: initialized_(false) {
	if (init) {
		Init();
	}
}

Mutex::~Mutex() {
	Destroy();
}


ThreadCond::ThreadCond(bool init): initial_(false), flag_(true)
{
	Init();

}

ThreadCond::~ThreadCond()
{
	sem_destroy(&sem_);

}


int ThreadCond::Init()
{
	printf("sem Initing \n");

    if( sem_init(&sem_, 0, 0) == -1 ) {
   		printf("ThreadCond::Init fail\n");
		assert(0);
   	} else {
		initial_ = true;
		flag_ = false;
		printf("sem Init ok \n");
	}
	return 0;
}

void ThreadCond::PostCond()
{
	printf("PostCond \n");
	sem_post(&sem_);
}


int ThreadCond::WaitCond(int second)
{
	struct timespec now;
	struct timespec outtime;

	clock_gettime(CLOCK_REALTIME, &now);

	outtime.tv_sec = now.tv_sec + second;
	outtime.tv_nsec = now.tv_nsec + 0;

	int ret = sem_timedwait(&sem_, &outtime);

	return ret;

}


int ThreadCond::WaitCond()
{
	return sem_wait(&sem_);
}




Runnable::Runnable() {
}

Runnable::~Runnable() {
}

Thread::Thread()
	: interrupted_(false)
	, alive_(false)
	, thread_(0)
	, runnable_(NULL) {
}

Thread::~Thread() {
	Join();

	if (runnable_ != NULL) {
		delete runnable_;
	}
}

void Thread::Kill() {
	if (alive_) {
		pthread_kill(thread_, 0);
	}
	alive_ = false;
}

void Thread::Join() {
	if (alive_) {
		void *result = NULL;
		pthread_join(thread_, &result);
		pthread_detach(thread_);
		thread_ = 0;
	}
	alive_ = false;

}

void Thread::Run() {
	runnable_->Run(this);
}

void *Thread::ThreadProc(void *arg) {
	((Thread *)arg)->Run();

	return NULL;
}



