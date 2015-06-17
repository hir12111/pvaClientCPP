/* pvaClientChannel.cpp */
/**
 * Copyright - See the COPYRIGHT that is included with this distribution.
 * EPICS pvData is distributed subject to a Software License Agreement found
 * in file LICENSE that is included with this distribution.
 */
/**
 * @author mrk
 * @date 2015.02
 */
#define epicsExportSharedSymbols

#include <map>
#include <sstream>
#include <pv/event.h>
#include <pv/lock.h>
#include <pv/pvaClient.h>
#include <pv/createRequest.h>


using std::tr1::static_pointer_cast;
using namespace epics::pvData;
using namespace epics::pvAccess;
using namespace std;

namespace epics { namespace pvaClient {


class PvaClientGetCache
{
public:
    PvaClientGetCache(){}
    ~PvaClientGetCache();
    void destroy() {
       pvaClientGetMap.clear();
    }
    PvaClientGetPtr getGet(string const & request);
    void addGet(string const & request,PvaClientGetPtr const & pvaClientGet);
private:
    map<string,PvaClientGetPtr> pvaClientGetMap;
};

PvaClientGetCache::~PvaClientGetCache()
{
    destroy();
}

PvaClientGetPtr PvaClientGetCache::getGet(string const & request)
{
    map<string,PvaClientGetPtr>::iterator iter = pvaClientGetMap.find(request);
    if(iter!=pvaClientGetMap.end()) return iter->second;
    return PvaClientGetPtr();
}

void PvaClientGetCache::addGet(string const & request,PvaClientGetPtr const & pvaClientGet)
{
     pvaClientGetMap.insert(std::pair<string,PvaClientGetPtr>(
         request,pvaClientGet));
}


class PvaClientPutCache
{
public:
    PvaClientPutCache(){}
    ~PvaClientPutCache();
    void destroy() {
       pvaClientPutMap.clear();
    }
    PvaClientPutPtr getPut(string const & request);
    void addPut(string const & request,PvaClientPutPtr const & pvaClientPut);
private:
    map<string,PvaClientPutPtr> pvaClientPutMap;
};

PvaClientPutCache::~PvaClientPutCache()
{
    destroy();
}

PvaClientPutPtr PvaClientPutCache::getPut(string const & request)
{
    map<string,PvaClientPutPtr>::iterator iter = pvaClientPutMap.find(request);
    if(iter!=pvaClientPutMap.end()) return iter->second;
    return PvaClientPutPtr();
}

void PvaClientPutCache::addPut(string const & request,PvaClientPutPtr const & pvaClientPut)
{
     pvaClientPutMap.insert(std::pair<string,PvaClientPutPtr>(
         request,pvaClientPut));
}

class ChannelRequesterImpl : public ChannelRequester
{
     PvaClientChannel *pvaClientChannel;
public:
     ChannelRequesterImpl(PvaClientChannel *pvaClientChannel)
     : pvaClientChannel(pvaClientChannel) {}
    void channelCreated(
        const Status& status,
        Channel::shared_pointer const & channel)
    { pvaClientChannel->channelCreated(status,channel); }
    void channelStateChange(
        Channel::shared_pointer const & channel,
        Channel::ConnectionState connectionState)
    {pvaClientChannel->channelStateChange(channel,connectionState);}
    tr1::shared_ptr<Channel> getChannel() {return pvaClientChannel->getChannel();}
    string getRequesterName()
    {return pvaClientChannel->getRequesterName();}
    void message(
        string const & message,
        MessageType messageType)
    { pvaClientChannel->message(message,messageType); }
    void destroy() {pvaClientChannel->destroy();}
};


PvaClientChannel::PvaClientChannel(
    PvaClientPtr const &pvaClient,
    string const & channelName,
    string const & providerName)
: pvaClient(pvaClient),
  channelName(channelName),
  providerName(providerName),
  connectState(connectIdle),
  isDestroyed(false),
  createRequest(CreateRequest::create()),
  pvaClientGetCache(new PvaClientGetCache()),
  pvaClientPutCache(new PvaClientPutCache())
{}

PvaClientChannel::~PvaClientChannel()
{
    destroy();
}

void PvaClientChannel::channelCreated(const Status& status, Channel::shared_pointer const & channel)
{
    if(isDestroyed) throw std::runtime_error("pvaClientChannel was destroyed");
    if(status.isOK()) {
        this->channel = channel;
        return;
    }
    cout << "PvaClientChannel::channelCreated status " << status.getMessage() << " why??\n";
}

void PvaClientChannel::channelStateChange(
    Channel::shared_pointer const & channel,
    Channel::ConnectionState connectionState)
{
    if(isDestroyed) return;
    bool waitingForConnect = false;
    if(connectState==connectActive) waitingForConnect = true;
    if(connectionState!=Channel::CONNECTED) {
         string mess(channelName +
             " connection state " + Channel::ConnectionStateNames[connectionState]);
         message(mess,errorMessage);
         channelConnectStatus = Status(Status::STATUSTYPE_ERROR,mess);
         connectState = notConnected;
    } else {
         connectState = connected;
    }
    if(waitingForConnect) waitForConnect.signal();
}

string PvaClientChannel::getRequesterName()
{
    PvaClientPtr yyy = pvaClient.lock();
    if(!yyy) throw std::runtime_error("PvaClient was destroyed");
    return yyy->getRequesterName();
}

void PvaClientChannel::message(
    string const & message,
    MessageType messageType)
{
    if(isDestroyed) throw std::runtime_error("pvaClientChannel was destroyed");
    PvaClientPtr yyy = pvaClient.lock();
    if(!yyy) throw std::runtime_error("PvaClient was destroyed");
    yyy->message(message, messageType);
}

void PvaClientChannel::destroy()
{
    {
        Lock xx(mutex);
        if(isDestroyed) return;
        isDestroyed = true;
    }
    if(channel) channel->destroy();
    channel.reset();
    pvaClientGetCache.reset();
    pvaClientPutCache.reset();
}

string PvaClientChannel::getChannelName()
{
    if(isDestroyed) throw std::runtime_error("pvaClientChannel was destroyed");
    return channelName;
}

Channel::shared_pointer PvaClientChannel::getChannel()
{
    if(isDestroyed) throw std::runtime_error("pvaClientChannel was destroyed");
    return channel;
}

void PvaClientChannel::connect(double timeout)
{
    if(isDestroyed) throw std::runtime_error("pvaClientChannel was destroyed");
    issueConnect();
    Status status = waitConnect(timeout);
    if(status.isOK()) return;
    stringstream ss;
    ss << "channel " << getChannelName() << " PvaClientChannel::connect " << status.getMessage();
    throw std::runtime_error(ss.str());
}

void PvaClientChannel::issueConnect()
{
    if(isDestroyed) throw std::runtime_error("pvaClientChannel was destroyed");
    if(connectState!=connectIdle) {
       throw std::runtime_error("pvaClientChannel already connected");
    }
    channelRequester = ChannelRequester::shared_pointer(new ChannelRequesterImpl(this));

    channelConnectStatus = Status(
           Status::STATUSTYPE_ERROR,
           getChannelName() + " createChannel failed");
    connectState = connectActive;
    ChannelProviderRegistry::shared_pointer reg = getChannelProviderRegistry();
    ChannelProvider::shared_pointer provider = reg->getProvider(providerName);
    if(!provider) {
        throw std::runtime_error(getChannelName() + " provider " + providerName + " not registered");
    }
    channel = provider->createChannel(channelName,channelRequester,ChannelProvider::PRIORITY_DEFAULT);
    if(!channel) {
         throw std::runtime_error(channelConnectStatus.getMessage());
    }
}

Status PvaClientChannel::waitConnect(double timeout)
{
    if(isDestroyed) throw std::runtime_error("pvaClientChannel was destroyed");
    waitForConnect.wait(timeout);
    if(connectState==connected) return Status::Ok;
    return Status(Status::STATUSTYPE_ERROR,channelConnectStatus.getMessage());
}

PvaClientFieldPtr PvaClientChannel::createField()
{
    return createField("");
}

PvaClientFieldPtr PvaClientChannel::createField(string const & subField)
{
    throw std::runtime_error("PvaClientChannel::createField not implemented");
}

PvaClientProcessPtr PvaClientChannel::createProcess()
{
    return createProcess("");
}

PvaClientProcessPtr PvaClientChannel::createProcess(string const & request)
{
    PVStructurePtr pvRequest = createRequest->createRequest(request);
    if(!pvRequest) {
        stringstream ss;
        ss << "channel " << getChannelName();
        ss << " PvaClientChannel::createProcess invalid pvRequest: " + createRequest->getMessage();
        throw std::runtime_error(ss.str());
    }
    return createProcess(pvRequest);
}

PvaClientProcessPtr PvaClientChannel::createProcess(PVStructurePtr const &  pvRequest)
{
    if(connectState!=connected) connect(5.0);
    if(connectState!=connected) throw std::runtime_error("PvaClientChannel::creatProcess not connected");
    PvaClientPtr yyy = pvaClient.lock();
    if(!yyy) throw std::runtime_error("PvaClient was destroyed");
    return PvaClientProcess::create(yyy,getPtrSelf(),channel,pvRequest);
}

PvaClientGetPtr PvaClientChannel::get() {return get("value,alarm,timeStamp");}

PvaClientGetPtr PvaClientChannel::get(string const & request)
{
    PvaClientGetPtr pvaClientGet = pvaClientGetCache->getGet(request);
    if(pvaClientGet) return pvaClientGet;
    pvaClientGet = createGet(request);
    pvaClientGet->connect();
    pvaClientGetCache->addGet(request,pvaClientGet);
    return pvaClientGet;
}

PvaClientGetPtr PvaClientChannel::createGet()
{
    return PvaClientChannel::createGet("value,alarm,timeStamp");
}

PvaClientGetPtr PvaClientChannel::createGet(string const & request)
{
    PVStructurePtr pvRequest = createRequest->createRequest(request);
    if(!pvRequest) {
        stringstream ss;
        ss << "channel " << getChannelName();
        ss << " PvaClientChannel::createGet invalid pvRequest: " + createRequest->getMessage();
        throw std::runtime_error(ss.str());
    }
    return createGet(pvRequest);
}

PvaClientGetPtr PvaClientChannel::createGet(PVStructurePtr const &  pvRequest)
{
    if(connectState!=connected) connect(5.0);
    if(connectState!=connected) throw std::runtime_error("PvaClientChannel::creatGet not connected");
    PvaClientPtr yyy = pvaClient.lock();
    if(!yyy) throw std::runtime_error("PvaClient was destroyed");
    return PvaClientGet::create(yyy,getPtrSelf(),channel,pvRequest);
}

PvaClientPutPtr PvaClientChannel::put() {return put("value");}

PvaClientPutPtr PvaClientChannel::put(string const & request)
{
    PvaClientPutPtr pvaClientPut = pvaClientPutCache->getPut(request);
    if(pvaClientPut) return pvaClientPut;
    pvaClientPut = createPut(request);
    pvaClientPut->connect();
    pvaClientPut->get();
    pvaClientPutCache->addPut(request,pvaClientPut);
    return pvaClientPut;
}

PvaClientPutPtr PvaClientChannel::createPut()
{
    return createPut("value");
}

PvaClientPutPtr PvaClientChannel::createPut(string const & request)
{
    PVStructurePtr pvRequest = createRequest->createRequest(request);
    if(!pvRequest) {
        stringstream ss;
        ss << "channel " << getChannelName();
        ss << " PvaClientChannel::createPut invalid pvRequest: " + createRequest->getMessage();
        throw std::runtime_error(ss.str());
    }
    return createPut(pvRequest);
}

PvaClientPutPtr PvaClientChannel::createPut(PVStructurePtr const & pvRequest)
{
    if(connectState!=connected) connect(5.0);
    if(connectState!=connected) throw std::runtime_error("PvaClientChannel::creatPut not connected");
    PvaClientPtr yyy = pvaClient.lock();
    if(!yyy) throw std::runtime_error("PvaClient was destroyed");
    return PvaClientPut::create(yyy,getPtrSelf(),channel,pvRequest);
}

PvaClientPutGetPtr PvaClientChannel::createPutGet()
{
    return createPutGet("putField(argument)getField(result)");
}

PvaClientPutGetPtr PvaClientChannel::createPutGet(string const & request)
{
    PVStructurePtr pvRequest = createRequest->createRequest(request);
    if(!pvRequest) {
        stringstream ss;
        ss << "channel " << getChannelName();
        ss << " PvaClientChannel::createPutGet invalid pvRequest: " + createRequest->getMessage();
        throw std::runtime_error(ss.str());
    }
    return createPutGet(pvRequest);
}

PvaClientPutGetPtr PvaClientChannel::createPutGet(PVStructurePtr const & pvRequest)
{
    if(connectState!=connected) connect(5.0);
    if(connectState!=connected) throw std::runtime_error("PvaClientChannel::creatPutGet not connected");
    PvaClientPtr yyy = pvaClient.lock();
    if(!yyy) throw std::runtime_error("PvaClient was destroyed");
    return PvaClientPutGet::create(yyy,getPtrSelf(),channel,pvRequest);
}

PvaClientRPCPtr PvaClientChannel::createRPC()
{
    return createRPC("");
}

PvaClientRPCPtr PvaClientChannel::createRPC(string const & request)
{
    PVStructurePtr pvRequest = createRequest->createRequest(request);
    if(!pvRequest) {
        stringstream ss;
        ss << "channel " << getChannelName();
        ss << " PvaClientChannel::createRPC invalid pvRequest: " + createRequest->getMessage();
        throw std::runtime_error(ss.str());
    }
    return createRPC(pvRequest);
}

PvaClientRPCPtr PvaClientChannel::createRPC(PVStructurePtr const & pvRequest)
{
    throw std::runtime_error("PvaClientChannel::createRPC not implemented");
}

PvaClientArrayPtr PvaClientChannel::createArray()
{
    return createArray("value");
}

PvaClientArrayPtr PvaClientChannel::createArray(string const & request)
{
    PVStructurePtr pvRequest = createRequest->createRequest(request);
    if(!pvRequest) {
        stringstream ss;
        ss << "channel " << getChannelName();
        ss << " PvaClientChannel::createArray invalid pvRequest: " + createRequest->getMessage();
        throw std::runtime_error(ss.str());
    }
    return createArray(pvRequest);
}

PvaClientArrayPtr PvaClientChannel::createArray(PVStructurePtr const &  pvRequest)
{
    throw std::runtime_error("PvaClientChannel::createArray not implemented");
}


PvaClientMonitorPtr PvaClientChannel::monitor() {return monitor("value,alarm,timeStamp");}

PvaClientMonitorPtr PvaClientChannel::monitor(string const & request)
{
    PvaClientMonitorPtr pvaClientMonitor = createMonitor(request);
    pvaClientMonitor->connect();
    pvaClientMonitor->start();
    return pvaClientMonitor;
}

PvaClientMonitorPtr PvaClientChannel::monitor(PvaClientMonitorRequesterPtr const & pvaClientMonitorRequester)
{    return monitor("value,alarm,timeStamp",pvaClientMonitorRequester);
}

PvaClientMonitorPtr PvaClientChannel::monitor(string const & request,
    PvaClientMonitorRequesterPtr const & pvaClientMonitorRequester)
{
    PvaClientMonitorPtr pvaClientMonitor = createMonitor(request);
    pvaClientMonitor->connect();
    pvaClientMonitor->setRequester(pvaClientMonitorRequester);
    pvaClientMonitor->start();
    return pvaClientMonitor;
}

PvaClientMonitorPtr PvaClientChannel::createMonitor()
{
    return createMonitor("value,alarm,timeStamp");
}

PvaClientMonitorPtr PvaClientChannel::createMonitor(string const & request)
{
    PVStructurePtr pvRequest = createRequest->createRequest(request);
    if(!pvRequest) {
        stringstream ss;
        ss << "channel " << getChannelName();
        ss << " PvaClientChannel::createMonitor invalid pvRequest: " + createRequest->getMessage();
        throw std::runtime_error(ss.str());
    }
    return createMonitor(pvRequest);
}

PvaClientMonitorPtr  PvaClientChannel::createMonitor(PVStructurePtr const &  pvRequest)
{
    if(connectState!=connected) connect(5.0);
    if(connectState!=connected) throw std::runtime_error("PvaClientChannel::createMonitor not connected");
    PvaClientPtr yyy = pvaClient.lock();
    if(!yyy) throw std::runtime_error("PvaClient was destroyed");
    return PvaClientMonitor::create(yyy,getPtrSelf(),channel,pvRequest);
}


PvaClientChannelPtr PvaClientChannel::create(
   PvaClientPtr const &pvaClient,
   string const & channelName,
   string const & providerName)
{
    PvaClientChannelPtr channel(new PvaClientChannel(pvaClient,channelName,providerName));
    return channel;
}

}}
