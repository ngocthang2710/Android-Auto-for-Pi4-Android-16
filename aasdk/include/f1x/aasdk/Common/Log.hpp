/*
*  This file is part of aasdk library project.
*  Copyright (C) 2018 f1x.studio (Michal Szwaj)
*
*  aasdk is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  aasdk is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with aasdk. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#ifdef __ANDROID__

#include <android/log.h>
#include <sstream>

namespace f1x
{
namespace aasdk
{
namespace common
{

class AndroidLogStream
{
public:
    AndroidLogStream(android_LogPriority priority)
        : priority_(priority)
    {
    }

    ~AndroidLogStream()
    {
        __android_log_print(priority_, "AaSdk", "%s", stream_.str().c_str());
    }

    template<typename T>
    AndroidLogStream& operator<<(const T& value)
    {
        stream_ << value;
        return *this;
    }

private:
    android_LogPriority priority_;
    std::ostringstream stream_;
};

}
}
}

#define AASDK_LOG_PRIORITY_trace ANDROID_LOG_VERBOSE
#define AASDK_LOG_PRIORITY_debug ANDROID_LOG_DEBUG
#define AASDK_LOG_PRIORITY_info ANDROID_LOG_INFO
#define AASDK_LOG_PRIORITY_warning ANDROID_LOG_WARN
#define AASDK_LOG_PRIORITY_error ANDROID_LOG_ERROR
#define AASDK_LOG_PRIORITY_fatal ANDROID_LOG_FATAL

#define AASDK_LOG(severity) f1x::aasdk::common::AndroidLogStream(static_cast<android_LogPriority>(AASDK_LOG_PRIORITY_##severity))

#else

#include <boost/log/trivial.hpp>

#define AASDK_LOG(severity) BOOST_LOG_TRIVIAL(severity) << "[AaSdk] "

#endif
