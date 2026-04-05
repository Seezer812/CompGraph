#pragma once
#include <windows.h>

class GameTimer
{
public:
    GameTimer();

    float TotalTime() const;
    float DeltaTime() const;

    void Reset();
    void Tick();

private:
    double mSecondsPerCount = 0.0;
    double mDeltaTime = 0.0;

    __int64 mBaseTime = 0;
    __int64 mPrevTime = 0;
};
