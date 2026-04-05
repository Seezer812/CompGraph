#include "GameTimer.h"

GameTimer::GameTimer()
{
    __int64 countsPerSec;
    QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
    mSecondsPerCount = 1.0 / (double)countsPerSec;
}

float GameTimer::TotalTime() const
{
    return (float)((mPrevTime - mBaseTime) * mSecondsPerCount);
}

float GameTimer::DeltaTime() const
{
    return (float)mDeltaTime;
}

void GameTimer::Reset()
{
    __int64 currTime;
    QueryPerformanceCounter((LARGE_INTEGER*)&currTime);

    mBaseTime = currTime;
    mPrevTime = currTime;
}

void GameTimer::Tick()
{
    __int64 currTime;
    QueryPerformanceCounter((LARGE_INTEGER*)&currTime);

    mDeltaTime = (currTime - mPrevTime) * mSecondsPerCount;
    mPrevTime = currTime;

    if (mDeltaTime < 0.0)
        mDeltaTime = 0.0;
}
