#include "AgentMcpStdio.h"
#include <QCoreApplication>
#include <QFile>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextStream>
#include <QFileInfo>

namespace noxshell {
namespace {
QJsonObject bridgeRequest(const QJsonObject &config,const QString &command)
{
    QLocalSocket socket;socket.connectToServer(config.value("socket").toString());
    if(!socket.waitForConnected(3000))return {{"error","玄壳 Agent 工作区已关闭。"}};
    QJsonObject request{{"token",config.value("token")}};
    if(!command.isNull())request.insert("command",command);
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+"\n");
    if(!socket.waitForBytesWritten(3000))return {{"error","无法发送服务器工具请求。"}};
    QByteArray response;
    while(!response.endsWith('\n')&&response.size()<300*1024){
        if(!socket.bytesAvailable()&&!socket.waitForReadyRead(180000))return {{"error","服务器工具响应超时。"}};
        response+=socket.readAll();
    }
    const auto doc=QJsonDocument::fromJson(response.left(response.indexOf('\n')));
    return doc.isObject()?doc.object():QJsonObject{{"error","服务器工具返回了无效响应。"}};
}
QJsonObject toolResult(const QString &text,bool error=false)
{
    return {{"content",QJsonArray{QJsonObject{{"type","text"},{"text",text}}}},{"isError",error}};
}
}
int runAgentMcpStdio(int argc,char **argv,const QString &bridgeFile)
{
    QCoreApplication app(argc,argv);
    QFile configFile(bridgeFile);
    if(!configFile.open(QIODevice::ReadOnly)||configFile.size()>4096)return 2;
    const auto config=QJsonDocument::fromJson(configFile.readAll()).object();
    if(config.value("socket").toString().isEmpty()||config.value("token").toString().size()<32)return 2;
    QFile input; if(!input.open(stdin,QIODevice::ReadOnly))return 3;
    QFile output; if(!output.open(stdout,QIODevice::WriteOnly))return 3;
    while(!input.atEnd()){
        const auto line=input.readLine(1024*1024+1);
        if(line.isEmpty())break;
        if(line.size()>1024*1024)return 4;
        const auto message=QJsonDocument::fromJson(line).object();
        const auto id=message.value("id");const auto method=message.value("method").toString();
        if(id.isUndefined())continue;
        QJsonObject response{{"jsonrpc","2.0"},{"id",id}};
        if(method=="initialize"){
            const auto requested=message.value("params").toObject().value("protocolVersion").toString();
            response.insert("result",QJsonObject{{"protocolVersion",requested.isEmpty()?"2024-11-05":requested},
                {"capabilities",QJsonObject{{"tools",QJsonObject{}}}},
                {"serverInfo",QJsonObject{{"name","noxshell-agent"},{"version","1"}}}});
        }else if(method=="ping")response.insert("result",QJsonObject{});
        else if(method=="tools/list"){
            response.insert("result",QJsonObject{{"tools",QJsonArray{QJsonObject{
                {"name","ssh_execute"},
                {"description","Execute one shell command on the current connected SSH server. The user confirms each command in NoxShell before it runs."},
                {"inputSchema",QJsonObject{{"type","object"},{"properties",QJsonObject{{"command",QJsonObject{{"type","string"},{"description","Shell command to run on the connected server"}}}}},{"required",QJsonArray{"command"}}}}
            }}}});
        }else if(method=="tools/call"){
            const auto params=message.value("params").toObject();
            if(params.value("name").toString()!="ssh_execute")response.insert("error",QJsonObject{{"code",-32602},{"message","Unknown tool"}});
            else {
                const auto command=params.value("arguments").toObject().value("command").toString();
                if(command.trimmed().isEmpty()||command.toUtf8().size()>8192)response.insert("result",toolResult("命令为空或超过 8192 字节。",true));
                else {
                    const auto result=bridgeRequest(config,command);
                    response.insert("result",toolResult(result.value("text").toString(result.value("error").toString()),result.contains("error")));
                }
            }
        }else if(method.startsWith("notifications/"))continue;
        else response.insert("error",QJsonObject{{"code",-32601},{"message","Method not found"}});
        output.write(QJsonDocument(response).toJson(QJsonDocument::Compact)+"\n");output.flush();
    }
    return 0;
}
}
