// See stderr_capture.h.

#include <stdafx.h>
#include "stderr_capture.h"
#include <os/logger.h>

#if defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>
#include <cerrno>
#endif

void StderrCapture::Start()
{
#if defined(__APPLE__)
    if (getenv("DR_STDERR_CAPTURE") != nullptr && atoi(getenv("DR_STDERR_CAPTURE")) == 0)
        return;
    int fds[2];
    if (pipe(fds) != 0)
        return;
    fflush(stderr);
    if (dup2(fds[1], STDERR_FILENO) < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    close(fds[1]);
    setvbuf(stderr, nullptr, _IONBF, 0);
    const int readEnd = fds[0];
    std::thread([readEnd] {
        pthread_setname_np("stderr");
        std::string line;
        char buffer[2048];
        while (true)
        {
            const ssize_t count = read(readEnd, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            for (ssize_t i = 0; i < count; i++)
            {
                if (buffer[i] == '\n' || buffer[i] == '\r')
                {
                    if (!line.empty())
                        os::logger::Log("[stderr] " + line);
                    line.clear();
                }
                else if (line.size() < 4000)
                    line += buffer[i];
            }
        }
    }).detach();
    LOGN("[stderr] stderr goes to this log (MoltenVK and plume messages; DR_STDERR_CAPTURE=0 off)");
#endif
}
