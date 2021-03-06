#ifndef TCPUDPSOCKET_H_INCLUDED
#define TCPUDPSOCKET_H_INCLUDED

#ifdef WIN32
#include <winsock2.h>
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x06000000
// #include <ws2def.h>
#include <windows.h>
#include <ws2tcpip.h>

#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define TRUE 1
#define FALSE 0
#endif

class TcpUdpSocket
{
  private:
#ifdef WIN32
    WSAData wsaData;
    SOCKET sock;
    SOCKET client;
#else
    int sock;
    int client;
#endif
    bool connected;
    long retval;
    sockaddr_in outaddr;
    char ip[30];
    char received[30];

  public:
    TcpUdpSocket(int port, char *address, bool udp, bool broadcast = true, bool reusesock = true, bool isServer = false,
                 int timeout = 0);
    ~TcpUdpSocket();

    int getRetVal();
    bool isConnected();

    bool wait();
    void disconnect();

    long receive(char *msg, int msgsize);
    char *received_from();
    long Write(const char *msg, int msgsize);
    long WriteTo(const char *msg, int msgsize, const char *name);
    int getAddress(const char *name, char *addr);
    const char *getAddress(const char *name);
};

#endif // DATAGRAMSOCKET_H_INCLUDED