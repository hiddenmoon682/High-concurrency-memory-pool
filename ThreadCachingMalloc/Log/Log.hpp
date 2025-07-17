#pragma once

#include <ctime>
#include <string>
#include <ctime>
#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <fstream>
#include <cstring>
#include <cstdarg>
#include <pthread.h>

class LockGuard
{
public:
    LockGuard(pthread_mutex_t *mutex):_mutex(mutex)
    {
        pthread_mutex_lock(_mutex);
    }
    ~LockGuard()
    {
        pthread_mutex_unlock(_mutex);
    }
private:
    pthread_mutex_t *_mutex;
};

namespace log_ns
{
    enum
    {
        DEBUG = 1,
        INFO,
        WARING,
        ERROR,
        FATAL
    };

    std::string LevelToString(int level)
    {
        switch (level)
        {
        case DEBUG:
            return "DEBUG";
        case INFO:
            return "INFO";
        case WARING:
            return "WARING";
        case ERROR:
            return "ERROR";
        case FATAL:
            return "FATAL";
        default:
            return "UNKOWN";
        }
    }

    std::string GetCurrtime()
    {
        time_t now = time(nullptr);
        struct tm *curr_time = localtime(&now);
        char buff[128];
        snprintf(buff, sizeof(buff), "%d-%02d-%02d %02d:%02d:%02d",
                 curr_time->tm_year + 1900,
                 curr_time->tm_mon + 1,
                 curr_time->tm_mday,
                 curr_time->tm_hour,
                 curr_time->tm_min,
                 curr_time->tm_sec);
        return buff;
    }

    class Logmessage
    {
    public:
        std::string _level;
        pid_t _id;
        std::string _filename;
        int _filenumber;
        std::string _cur_time;
        std::string _message_info;
    };

#define SCREEN_TYPE 1
#define FILE_TYPE 2

    const std::string glogfile = "./log.txt";
    pthread_mutex_t glock = PTHREAD_MUTEX_INITIALIZER;

    class Log
    {
    public:
        Log(const std::string filename = glogfile):_logfile(filename), _type(SCREEN_TYPE)
        {
        }

        void Enable(int type)
        {
            _type = type;
        }

        void FlushToScreen(const Logmessage& log)
        {
            printf("[%s][%d][%s][%d][%s]:%s",
                log._level.c_str(),
                log._id,
                log._filename.c_str(),
                log._filenumber,
                log._cur_time.c_str(),
                log._message_info.c_str());
        }

        void FlushToFile(const Logmessage& log)
        {
            std::ofstream out(_logfile, std::ios::app);
            if (!out.is_open())
                return;
            char logtxt[2048];
            snprintf(logtxt, sizeof(logtxt), "[%s][%d][%s][%d][%s] %s",
                     log._level.c_str(),
                     log._id,
                     log._filename.c_str(),
                     log._filenumber,
                     log._cur_time.c_str(),
                     log._message_info.c_str());
            out.write(logtxt, strlen(logtxt));
            out.close();
        }

        void FlushLog(const Logmessage &lg)
        {
            // 加过滤逻辑 --- TODO

            LockGuard lockguard(&glock);
            switch (_type)
            {
            case SCREEN_TYPE:
                FlushToScreen(lg);
                break;
            case FILE_TYPE:
                FlushToFile(lg);
                break;
            }
        }

        void LogMessage(std::string filename, int filenumber, int level, const char* format, ...)
        {
            Logmessage lg;
            lg._filename = filename;
            lg._filenumber = filenumber;
            lg._level = LevelToString(level);
            lg._id = getpid();
            lg._cur_time = GetCurrtime();

            va_list ap;
            va_start(ap, format);
            char log_info[1024];
            vsnprintf(log_info, sizeof(log_info), format, ap);
            va_end(ap);
            lg._message_info = log_info;

            FlushLog(lg);
        }

        ~Log()
        {}
    private:
        std::string _logfile;
        int _type; 
    };

    Log lg;

// __FILE__ 是一个表示当前文件名的宏
// __LINE__ 是一个表示当前行号的宏
// ## 是一个粘滞符，在这里可以保证即使可变参数包是空的也不会报错
// __VA_ARGS__ 是一个用在宏函数里表示可变参数包的宏

#define LOG(level, Format, ...) \
    do                          \
    {                           \
        lg.LogMessage(__FILE__, __LINE__, level, Format, ##__VA_ARGS__);  \
    } while (0);
    
#define EnableScreen()          \
    do                          \
    {                           \
        lg.Enable(SCREEN_TYPE); \
    } while (0)
#define EnableFILE()          \
    do                        \
    {                         \
        lg.Enable(FILE_TYPE); \
    } while (0)


};

