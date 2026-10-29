#include "ProxyTunnel.h"
#include <QElapsedTimer>
#include <QUrl>
#include <QHostAddress>
#ifdef Q_OS_WIN
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
#endif

namespace noxshell {
bool negotiateProxyTunnel(qintptr descriptor, const QNetworkProxy &proxy, const QString &host,
    quint16 port, int timeoutMs, const std::function<bool()> &canceled, QString &error)
{
    QElapsedTimer clock; clock.start();
    error = QStringLiteral("代理协商失败或超时");
#ifdef Q_OS_WIN
    const auto socket = static_cast<SOCKET>(descriptor);
    u_long mode = 1; if (ioctlsocket(socket, FIONBIO, &mode) != 0) return false;
    struct Restore { SOCKET socket; ~Restore(){u_long mode=0;ioctlsocket(socket,FIONBIO,&mode);} } restore{socket};
#else
    const int socket = static_cast<int>(descriptor);
    const int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0 || fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0) return false;
    struct Restore { int socket,flags; ~Restore(){fcntl(socket,F_SETFL,flags);} } restore{socket,flags};
#endif
    auto ready = [&](bool write) {
        while (!canceled() && clock.elapsed() < timeoutMs) {
            const int wait = qMin(100, timeoutMs-static_cast<int>(clock.elapsed()));
#ifdef Q_OS_WIN
            fd_set set;FD_ZERO(&set);FD_SET(socket,&set);timeval t{0,wait*1000};
            const int result=select(0,write?nullptr:&set,write?&set:nullptr,nullptr,&t);
            if(result>0)return true;
            if(result<0&&WSAGetLastError()!=WSAEINTR)return false;
#else
            pollfd p{socket,static_cast<short>(write?POLLOUT:POLLIN),0};
            const int result=::poll(&p,1,wait);
            if(result>0)return (p.revents&(write?POLLOUT:POLLIN))!=0;
            if(result<0&&errno!=EINTR)return false;
#endif
        }
        return false;
    };
    auto send = [&](const QByteArray &bytes) {
        qsizetype offset=0;
        while(offset<bytes.size()) {
            if(!ready(true))return false;
#ifdef MSG_NOSIGNAL
            constexpr int options=MSG_NOSIGNAL;
#else
            constexpr int options=0;
#endif
            const int count=::send(socket,bytes.constData()+offset,static_cast<int>(qMin<qsizetype>(4096,bytes.size()-offset)),options);
            if(count<=0)return false;
            offset+=count;
        }
        return true;
    };
    auto read = [&](int length,QByteArray &bytes) {
        bytes.clear();
        while(bytes.size()<length) {
            if(!ready(false))return false;
            char buffer[512];const int count=::recv(socket,buffer,qMin<int>(sizeof(buffer),length-bytes.size()),0);
            if(count<=0)return false;
            bytes.append(buffer,count);
        }
        return true;
    };
    if(host.contains('\r')||host.contains('\n')||host.contains(' ')||host.isEmpty())return false;
    if(proxy.type()==QNetworkProxy::Socks5Proxy) {
        const auto user=proxy.user().toUtf8(),password=proxy.password().toUtf8();
        if(user.size()>255||password.size()>255){error=QStringLiteral("SOCKS5 凭据过长");return false;}
        const bool auth=!user.isEmpty()||!password.isEmpty();
        QByteArray reply;
        if(!send(auth?QByteArray::fromHex("05020002"):QByteArray::fromHex("050100"))||!read(2,reply)||reply[0]!=5)return false;
        if(reply[1]==2&&auth) {
            QByteArray request(1,'\1');request.append(char(user.size())).append(user).append(char(password.size())).append(password);
            if(!send(request)||!read(2,reply)||reply[0]!=1||reply[1]!=0){error=QStringLiteral("SOCKS5 代理认证失败");return false;}
        } else if(reply[1]!=0){error=QStringLiteral("SOCKS5 代理不支持请求的认证方式");return false;}
        // Remote DNS: avoid resolving the SSH endpoint outside the proxy.
        QByteArray request=QByteArray::fromHex("050100");
        const QHostAddress address(host);
        if(address.protocol()==QAbstractSocket::IPv6Protocol) {
            request.append(char(4));const auto bytes=address.toIPv6Address();
            request.append(reinterpret_cast<const char*>(bytes.c),16);
        } else if(address.protocol()==QAbstractSocket::IPv4Protocol) {
            request.append(char(1));const auto ip=address.toIPv4Address();
            for(int shift=24;shift>=0;shift-=8)request.append(char((ip>>shift)&255));
        } else {
            const auto target=QUrl::toAce(host);if(target.isEmpty()||target.size()>255)return false;
            request.append(char(3)).append(char(target.size())).append(target);
        }
        request.append(char(port>>8)).append(char(port&255));
        if(!send(request)||!read(4,reply)||reply[0]!=5||reply[1]!=0){error=QStringLiteral("SOCKS5 代理拒绝目标连接");return false;}
        const auto type=quint8(reply[3]);int length=type==1?4:type==4?16:0;
        if(type==3){if(!read(1,reply))return false;length=quint8(reply[0]);}
        if(!length||!read(length+2,reply))return false;
    } else if(proxy.type()==QNetworkProxy::HttpProxy) {
        const auto authority=(host.contains(':')?"["+host+"]":host)+":"+QString::number(port);
        QByteArray request="CONNECT "+authority.toUtf8()+" HTTP/1.1\r\nHost: "+authority.toUtf8()+"\r\n";
        if(!proxy.user().isEmpty()||!proxy.password().isEmpty())request+="Proxy-Authorization: Basic "+(proxy.user()+":"+proxy.password()).toUtf8().toBase64()+"\r\n";
        request+="\r\n";if(!send(request))return false;
        QByteArray headers,byte;
        // Read exactly through the header; preserve any SSH banner in the socket.
        while(!headers.endsWith("\r\n\r\n")&&headers.size()<16384){if(!read(1,byte))return false;headers+=byte;}
        const auto status=headers.left(headers.indexOf("\r\n")).split(' ');
        if(!headers.endsWith("\r\n\r\n")||status.size()<2||!status[0].startsWith("HTTP/")||status[1]!="200"){
            error=QStringLiteral("HTTP CONNECT 代理拒绝目标连接");return false;
        }
    } else return false;
    error.clear();return true;
}
}
