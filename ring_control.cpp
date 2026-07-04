// SeeTrue Task 1 "Eye Ring" host-side control example.
//
// Opens the Pico's USB CDC serial port, sends a few commands, prints
// replies. One file, two serial backends selected at compile time.
//
// Build (Linux/macOS):
//   g++ -std=c++17 -O2 -o ring_control ring_control.cpp
// Build (Windows, MSVC):
//   cl /EHsc /std:c++17 ring_control.cpp
//
// Run:
//   Linux:   ./ring_control /dev/ttyACM0
//   macOS:   ./ring_control /dev/tty.usbmodemXXXX
//   Windows: ring_control.exe COM5
//
// The device speaks a line-based ASCII protocol. USB CDC ignores the baud
// rate, so the value below is nominal.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ------------------------------------------------------------------
// Serial abstraction
// ------------------------------------------------------------------
class Serial {
public:
    explicit Serial(const std::string& port);
    ~Serial();

    bool ok() const { return open_; }
    bool writeLine(const std::string& s);   // appends '\n'
    std::string readLine(int timeout_ms = 500);

private:
    bool open_ = false;
#ifdef _WIN32
    void* handle_ = nullptr;   // HANDLE
#else
    int fd_ = -1;
#endif
};

// ------------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>

Serial::Serial(const std::string& port) {
    // "\\\\.\\COMn" form works for COM10 and above too.
    std::string path = "\\\\.\\" + port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "open %s failed (err %lu)\n",
                     port.c_str(), GetLastError());
        return;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = CBR_115200;   // nominal, CDC ignores it
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(h, &dcb);

    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout        = 50;
    to.ReadTotalTimeoutConstant   = 500;
    to.ReadTotalTimeoutMultiplier = 10;
    to.WriteTotalTimeoutConstant  = 500;
    SetCommTimeouts(h, &to);

    handle_ = h;
    open_   = true;
}

Serial::~Serial() {
    if (open_) CloseHandle((HANDLE)handle_);
}

bool Serial::writeLine(const std::string& s) {
    std::string out = s + "\n";
    DWORD written = 0;
    return WriteFile((HANDLE)handle_, out.data(),
                     (DWORD)out.size(), &written, nullptr)
           && written == out.size();
}

std::string Serial::readLine(int /*timeout_ms*/) {
    std::string line;
    char c;
    DWORD n = 0;
    while (ReadFile((HANDLE)handle_, &c, 1, &n, nullptr) && n == 1) {
        if (c == '\n') break;
        if (c != '\r') line.push_back(c);
    }
    return line;
}

// ------------------------------------------------------------------
#else   // POSIX
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>

Serial::Serial(const std::string& port) {
    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        std::fprintf(stderr, "open %s failed: %s\n",
                     port.c_str(), std::strerror(errno));
        return;
    }

    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        std::fprintf(stderr, "tcgetattr failed: %s\n", std::strerror(errno));
        ::close(fd);
        return;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);   // nominal, CDC ignores it
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);

    // back to blocking with select-based timeouts
    fcntl(fd, F_SETFL, 0);

    fd_   = fd;
    open_ = true;
}

Serial::~Serial() {
    if (open_) ::close(fd_);
}

bool Serial::writeLine(const std::string& s) {
    std::string out = s + "\n";
    size_t total = 0;
    while (total < out.size()) {
        ssize_t w = ::write(fd_, out.data() + total, out.size() - total);
        if (w < 0) return false;
        total += (size_t)w;
    }
    return true;
}

std::string Serial::readLine(int timeout_ms) {
    std::string line;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd_, &rd);
        timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int r = select(fd_ + 1, &rd, nullptr, nullptr, &tv);
        if (r <= 0) break;   // timeout or error

        char c;
        ssize_t n = ::read(fd_, &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        if (c != '\r') line.push_back(c);
    }
    return line;
}
#endif

// ------------------------------------------------------------------
// Small helper: send a command, print the reply lines until "OK"/"ERR".
// ------------------------------------------------------------------
static void command(Serial& s, const std::string& cmd) {
    std::printf(">>> %s\n", cmd.c_str());
    if (!s.writeLine(cmd)) {
        std::printf("    (write failed)\n");
        return;
    }
    for (int i = 0; i < 16; i++) {
        std::string r = s.readLine(500);
        if (r.empty()) break;
        std::printf("    %s\n", r.c_str());
        if (r == "OK" || r.rfind("ERR", 0) == 0) break;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <serial-port>\n"
            "  Linux:   %s /dev/ttyACM0\n"
            "  macOS:   %s /dev/tty.usbmodemXXXX\n"
            "  Windows: %s COM5\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    Serial s(argv[1]);
    if (!s.ok()) return 1;

    // A short demo sequence exercising the protocol.
    command(s, "IDN?");        // expect SEETRUE-RING v1.0
    command(s, "LED ALL 0");   // start dark
    command(s, "LED 1 40");    // one channel at 40%
    command(s, "LED 4 75");    // another at 75%
    command(s, "ON 1");
    command(s, "ON 4");
    command(s, "SYNC 60");     // 60 Hz sync out
    command(s, "GET");         // read back state
    command(s, "SAVE");        // persist to flash

    return 0;
}
