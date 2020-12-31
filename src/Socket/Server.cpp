
#include <nstd/Socket/Server.hpp>
#include <nstd/Socket/Socket.hpp>
#include <nstd/MultiMap.hpp>
#include <nstd/PoolList.hpp>
#include <nstd/List.hpp>
#include <nstd/Time.hpp>
#include <nstd/Buffer.hpp>
#include <nstd/Error.hpp>
#include <nstd/Mutex.hpp>

class Server::Private
{
public:
  struct ListenerImpl : public Socket
  {
    Listener::ICallback *callback;
  };

  class ClientImpl : public Socket
  {
  public:
    Client::ICallback *_callback;
    Buffer _sendBuffer;
    bool _suspended;
    Server::Private *_p;
    ClientImpl() : _suspended(false) {}
    bool write(const byte *data, usize size, usize *postponed = 0);
    bool read(byte *buffer, usize maxSize, usize &size);
    void suspend();
    void resume();
  };

  struct EstablisherImpl : public Socket
  {
    Establisher::ICallback *callback;
  };

  struct TimerImpl
  {
    Timer::ICallback *callback;
    int64 executionTime;
    int64 interval;
  };

public:
  Private();

  void setKeepAlive(bool enable) { _keepAlive = enable; }
  void setNoDelay(bool enable) { _noDelay = enable; }
  void setSendBufferSize(int size) { _sendBufferSize = size; }
  void setReceiveBufferSize(int size) { _receiveBufferSize = size; }
  void setReuseAddress(bool enable) { _reuseAddress = enable; }

  Listener *listen(uint32 addr, uint16 port, Listener::ICallback &callback);
  Establisher *connect(uint32 addr, uint16 port, Establisher::ICallback &callback);
  Timer *time(int64 interval, Timer::ICallback &callback);
  Client *pair(Client::ICallback &callback, Socket &socket);

  void remove(Client &client);
  void remove(Listener &listener);
  void remove(Establisher &establisher);
  void remove(Timer &timer);

  void run();
  void interrupt();

  void clear();

private:
  bool _keepAlive;
  bool _noDelay;
  int _sendBufferSize;
  int _receiveBufferSize;
  bool _reuseAddress;

  Socket::Poll _sockets;
  MultiMap<int64, TimerImpl *> _queuedTimers;
  PoolList<ListenerImpl> _listeners;
  PoolList<EstablisherImpl> _establishers;
  PoolList<ClientImpl> _clients;
  PoolList<TimerImpl> _timers;

  List<ClientImpl *> _closingClients;

  Mutex _interruptMutex;
  bool _interrupted;
};

Server::Private::Private() : _keepAlive(false), _noDelay(false), _sendBufferSize(0), _receiveBufferSize(0), _reuseAddress(true), _interrupted(false)
{
  _queuedTimers.insert(0, 0); // add default timeout timer
}

Server::Listener *Server::Private::listen(uint32 addr, uint16 port, Listener::ICallback &callback)
{
  Socket socket;
  if (!socket.open() ||
      (_reuseAddress && !socket.setReuseAddress()) ||
      !socket.bind(addr, port) ||
      !socket.listen())
    return 0;
  ListenerImpl &listener = _listeners.append();
  listener.swap(socket);
  listener.callback = &callback;
  _sockets.set(listener, Socket::Poll::acceptFlag);
  return (Server::Listener *)&listener;
}

Server::Establisher *Server::Private::connect(uint32 addr, uint16 port, Establisher::ICallback &callback)
{
  Socket socket;
  if (!socket.open() ||
      !socket.setNonBlocking() ||
      !socket.connect(addr, port))
    return 0;
  EstablisherImpl &establisher = _establishers.append();
  establisher.swap(socket);
  establisher.callback = &callback;
  _sockets.set(establisher, Socket::Poll::connectFlag);
  return (Server::Establisher *)&establisher;
}

Server::Timer *Server::Private::time(int64 interval, Timer::ICallback &callback)
{
  TimerImpl &timer = _timers.append();
  timer.callback = &callback;
  timer.executionTime = Time::ticks() + interval;
  timer.interval = interval;
  _queuedTimers.insert(timer.executionTime, &timer);
  return (Server::Timer *)&timer;
}

Server::Client *Server::Private::pair(Client::ICallback &callback, Socket &otherSocket)
{
  Socket socket;
  if (!socket.pair(otherSocket) ||
      !socket.setNonBlocking() ||
      (_keepAlive && !socket.setKeepAlive()) ||
      (_noDelay && !socket.setNoDelay()) ||
      (_sendBufferSize > 0 && !socket.setSendBufferSize(_sendBufferSize)) ||
      (_receiveBufferSize > 0 && !socket.setReceiveBufferSize(_receiveBufferSize)))
  {
    otherSocket.close();
    return 0;
  }
  ClientImpl &client = _clients.append();
  client._p = this;
  client.swap(socket);
  _sockets.set(client, Socket::Poll::readFlag);
  return (Server::Client *)&client;
}

void Server::Private::remove(Client &client_)
{
  ClientImpl &client = *(ClientImpl *)&client_;
  _sockets.remove(client);
  _clients.remove(client);
}

void Server::Private::remove(Listener &listener_)
{
  ListenerImpl &listener = *(ListenerImpl *)&listener_;
  _sockets.remove(listener);
  _listeners.remove(listener);
}

void Server::Private::remove(Establisher &establisher_)
{
  EstablisherImpl &establisher = *(EstablisherImpl *)&establisher_;
  _sockets.remove(establisher);
  _establishers.remove(establisher);
}

void Server::Private::remove(Timer &timer_)
{
  TimerImpl &timer = *(TimerImpl *)&timer_;
  for (MultiMap<int64, TimerImpl *>::Iterator i = _queuedTimers.find(timer.executionTime), end = _queuedTimers.end(); i != end; ++i)
  {
    if (*i == &timer)
    {
      _queuedTimers.remove(i);
      break;
    }
    if (i.key() != timer.executionTime)
      break;
  }
  _timers.remove(timer);
}

void Server::Private::run()
{
  for (Socket::Poll::Event pollEvent;;)
  {
    int64 now = Time::ticks();
    int64 timeout = _queuedTimers.begin().key() - now;
    for (; timeout <= 0; timeout = _queuedTimers.begin().key() - now)
    {
      TimerImpl *timer = _queuedTimers.front();
      _queuedTimers.removeFront();
      if (timer) // user timer
      {
        timer->executionTime += timer->interval;
        _queuedTimers.insert(timer->executionTime, timer);
        timer->callback->onActivated();
      }
      else
        _queuedTimers.insert(now + 300 * 1000, 0); // keep "default timeout" timer
    }

    while (!_closingClients.isEmpty())
    {
      ClientImpl &client = *_closingClients.front();
      _closingClients.removeFront();
      client._callback->onClosed();
    }

    if (!_sockets.poll(pollEvent, timeout))
      break;

    if (!pollEvent.flags)
    {
      if (_interrupted)
      {
        Mutex::Guard guard(_interruptMutex);
        if (_interrupted)
        {
          _interrupted = false;
          return;
        }
      }
      continue; // timeout
    }

    if (pollEvent.flags & Socket::Poll::readFlag)
      ((ClientImpl *)pollEvent.socket)->_callback->onRead();
    else if (pollEvent.flags & Socket::Poll::writeFlag)
    {
      ClientImpl &client = *(ClientImpl *)pollEvent.socket;
      if (!client._sendBuffer.isEmpty())
      {
        ssize sent = client.send(client._sendBuffer, client._sendBuffer.size());
        switch (sent)
        {
        case -1:
          if (Socket::getLastError() == 0) // EWOULDBLOCK
            continue;
          // no break
        case 0:
          client._sendBuffer.free();
          _sockets.remove(client);
          client._callback->onClosed();
          continue;
        default:
          break;
        }
        client._sendBuffer.removeFront((usize)sent);
      }
      if (client._sendBuffer.isEmpty())
      {
        client._sendBuffer.free();
        if (client._suspended)
          _sockets.remove(client);
        else
          _sockets.set(client, Socket::Poll::readFlag);
        client._callback->onWrite();
      }
      continue;
    }
    else if (pollEvent.flags & Socket::Poll::acceptFlag)
    {
      ListenerImpl &listener = *(ListenerImpl *)pollEvent.socket;
      Socket clientSocket;
      uint32 ip;
      uint16 port;
      if (!listener.accept(clientSocket, ip, port) ||
          !clientSocket.setNonBlocking() ||
          (_keepAlive && !clientSocket.setKeepAlive()) ||
          (_noDelay && !clientSocket.setNoDelay()) ||
          (_sendBufferSize > 0 && !clientSocket.setSendBufferSize(_sendBufferSize)) ||
          (_receiveBufferSize > 0 && !clientSocket.setReceiveBufferSize(_receiveBufferSize)))
        continue;
      ClientImpl &client = _clients.append();
      client._p = this;
      client.swap(clientSocket);
      _sockets.set(client, Socket::Poll::readFlag);
      client._callback = listener.callback->onAccepted(*(Client *)&client, ip, port);
      if (!client._callback)
        _clients.remove(client);
      continue;
    }
    else if (pollEvent.flags & Socket::Poll::connectFlag)
    {
      EstablisherImpl &establisher = *(EstablisherImpl *)pollEvent.socket;
      _sockets.remove(establisher);
      int error = establisher.getAndResetErrorStatus();
      if (error)
      {
        Error::setLastError((uint)error);
        establisher.callback->onAbolished();
      }
      else
      {
        if ((_keepAlive && !establisher.setKeepAlive()) ||
            (_noDelay && !establisher.setNoDelay()) ||
            (_sendBufferSize > 0 && !establisher.setSendBufferSize(_sendBufferSize)) ||
            (_receiveBufferSize > 0 && !establisher.setReceiveBufferSize(_receiveBufferSize)))
          establisher.callback->onAbolished();
        else
        {
          ClientImpl &client = _clients.append();
          client._p = this;
          client.swap(establisher);
          _sockets.set(client, Socket::Poll::readFlag);
          client._callback = establisher.callback->onConnected(*(Client *)&client);
          if (!client._callback)
            _clients.remove(client);
        }
      }
      continue;
    }
  }
}

void Server::Private::interrupt()
{
  {
    Mutex::Guard guard(_interruptMutex);
    if (_interrupted)
      return;
    _interrupted = true;
  }
  _sockets.interrupt();
}

void Server::Private::clear()
{
  _sockets.clear();
  _queuedTimers.clear();
  _listeners.clear();
  _establishers.clear();
  _clients.clear();
  _timers.clear();
  _closingClients.clear();
  _queuedTimers.insert(0, 0);
  _interrupted = false;
}

bool Server::Private::ClientImpl::write(const byte *data, usize size, usize *postponed)
{
  if (_sendBuffer.isEmpty())
  {
    ssize sent = send(data, size);
    switch (sent)
    {
    case -1:
      if (Socket::getLastError() == 0) // EWOULDBLOCK
      {
        sent = 0;
        break;
      }
      // no break
    case 0:
      _p->_closingClients.append(this);
      return false;
    default:
      break;
    }
    if ((usize)sent >= size)
    {
      if (postponed)
        *postponed = 0;
      return true;
    }
    _sendBuffer.append(data + sent, size - sent);
    _p->_sockets.set(*this, Socket::Poll::writeFlag);
  }
  else
    _sendBuffer.append(data, size);
  if (postponed)
    *postponed = _sendBuffer.size();
  return true;
}

bool Server::Private::ClientImpl::read(byte *buffer, usize maxSize, usize &size)
{
  ssize received = recv(buffer, maxSize);
  switch (received)
  {
  case -1:
    if (Socket::getLastError() == 0) // EWOULDBLOCK
    {
      size = 0;
      return false;
    }
    // no break
  case 0:
    _p->_closingClients.append(this);
    size = 0;
    return false;
  default:
    break;
  }
  size = (usize)received;
  return true;
}

void Server::Private::ClientImpl::suspend()
{
  _suspended = true;
  if (_sendBuffer.isEmpty())
    _p->_sockets.remove(*this);
}

void Server::Private::ClientImpl::resume()
{
  _suspended = false;
  if (_sendBuffer.isEmpty())
    _p->_sockets.set(*this, Socket::Poll::readFlag);
}

Server::Server() : _p(new Private) {}
Server::~Server() { delete _p; }
void Server::setKeepAlive(bool enable) { return _p->setKeepAlive(enable); }
void Server::setNoDelay(bool enable) { return _p->setNoDelay(enable); }
void Server::setSendBufferSize(int size) { return _p->setSendBufferSize(size); }
void Server::setReceiveBufferSize(int size) { return _p->setReceiveBufferSize(size); }
void Server::setReuseAddress(bool enable) { return _p->setReuseAddress(enable); }
Server::Listener *Server::listen(uint32 addr, uint16 port, Listener::ICallback &callback) { return _p->listen(addr, port, callback); }
Server::Establisher *Server::connect(uint32 addr, uint16 port, Establisher::ICallback &callback) { return _p->connect(addr, port, callback); }
Server::Timer *Server::time(int64 interval, Timer::ICallback &callback) { return _p->time(interval, callback); }
Server::Client *Server::pair(Client::ICallback &callback, Socket &socket) { return _p->pair(callback, socket); }
void Server::remove(Server::Client &client) { return _p->remove(client); }
void Server::remove(Server::Listener &listener) { return _p->remove(listener); }
void Server::remove(Server::Establisher &establisher) { return _p->remove(establisher); }
void Server::remove(Server::Timer &timer) { return _p->remove(timer); }
void Server::run() { return _p->run(); }
void Server::interrupt() { return _p->interrupt(); }
void Server::clear() { return _p->clear(); }
bool Server::Client::write(const byte *data, usize size, usize *postponed) { return ((Private::ClientImpl *)this)->write(data, size, postponed); }
bool Server::Client::read(byte *buffer, usize maxSize, usize &size) { return ((Private::ClientImpl *)this)->read(buffer, maxSize, size); }
void Server::Client::suspend() { return ((Private::ClientImpl *)this)->suspend(); }
void Server::Client::resume() { return ((Private::ClientImpl *)this)->resume(); }
