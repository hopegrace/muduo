// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/TcpConnection.h>

#include <muduo/base/Logging.h>
#include <muduo/base/WeakCallback.h>
#include <muduo/net/Channel.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Socket.h>
#include <muduo/net/SocketsOps.h>

#include <errno.h>

using namespace muduo;
using namespace muduo::net;

void muduo::net::defaultConnectionCallback(const TcpConnectionPtr& conn)
{
  LOG_TRACE << conn->localAddress().toIpPort() << " -> "
            << conn->peerAddress().toIpPort() << " is "
            << (conn->connected() ? "UP" : "DOWN");
  // do not call conn->forceClose(), because some users want to register message callback only.
}

void muduo::net::defaultMessageCallback(const TcpConnectionPtr&,
                                        Buffer* buf,
                                        Timestamp)
{
  buf->retrieveAll();
}

TcpConnection::TcpConnection(EventLoop* loop,
                             const string& nameArg,
                             int sockfd,
                             const InetAddress& localAddr,
                             const InetAddress& peerAddr)
  : loop_(CHECK_NOTNULL(loop)), //检查loop指针是否为NULL
    name_(nameArg),
    state_(kConnecting),
    reading_(true),
    socket_(new Socket(sockfd)),//Acceptor返回的新连接fd
    channel_(new Channel(loop, sockfd)),//该连接对应的Channel
    localAddr_(localAddr),
    peerAddr_(peerAddr),
    highWaterMark_(64*1024*1024)
{
  //设置事件的回调
  channel_->setReadCallback(
      std::bind(&TcpConnection::handleRead, this, _1));
  channel_->setWriteCallback(
      std::bind(&TcpConnection::handleWrite, this));
  channel_->setCloseCallback(
      std::bind(&TcpConnection::handleClose, this));
  channel_->setErrorCallback(
      std::bind(&TcpConnection::handleError, this));
  LOG_DEBUG << "TcpConnection::ctor[" <<  name_ << "] at " << this
            << " fd=" << sockfd;
  socket_->setKeepAlive(true);//开启socket的SO_KEEPALIVE选项
}

TcpConnection::~TcpConnection()
{
  LOG_DEBUG << "TcpConnection::dtor[" <<  name_ << "] at " << this
            << " fd=" << channel_->fd()
            << " state=" << stateToString();
  assert(state_ == kDisconnected);
}

bool TcpConnection::getTcpInfo(struct tcp_info* tcpi) const
{
  return socket_->getTcpInfo(tcpi);
}

string TcpConnection::getTcpInfoString() const
{
  char buf[1024];
  buf[0] = '\0';
  socket_->getTcpInfoString(buf, sizeof buf);
  return buf;
}

//StringPiece是对数据的封装,成员包括const char*和数据长度int
void TcpConnection::send(const void* data, int len)
{
  send(StringPiece(static_cast<const char*>(data), len));
}

void TcpConnection::send(const StringPiece& message)
{
  if (state_ == kConnected)
  {
    if (loop_->isInLoopThread())
    {
      //如果实在IO线程的loop中直接发送
      sendInLoop(message);
    }
    else //否则将任务转移给IO线程
    {
      void (TcpConnection::*fp)(const StringPiece& message) = &TcpConnection::sendInLoop;
      loop_->runInLoop(
          std::bind(fp,
                    this,     // FIXME
                    message.as_string()));
                    //std::forward<string>(message)));
    }
  }
}

// FIXME efficiency!!!
void TcpConnection::send(Buffer* buf)
{
  if (state_ == kConnected)
  {
    if (loop_->isInLoopThread())
    {
      sendInLoop(buf->peek(), buf->readableBytes());
      buf->retrieveAll();
    }
    else
    {
      void (TcpConnection::*fp)(const StringPiece& message) = &TcpConnection::sendInLoop;
      loop_->runInLoop(
          std::bind(fp,
                    this,     // FIXME
                    buf->retrieveAllAsString()));
                    //std::forward<string>(message)));
    }
  }
}

void TcpConnection::sendInLoop(const StringPiece& message)
{
  sendInLoop(message.data(), message.size());
}

void TcpConnection::sendInLoop(const void* data, size_t len)
{
  loop_->assertInLoopThread();
  ssize_t nwrote = 0;
  size_t remaining = len;
  bool faultError = false;
  if (state_ == kDisconnected) //如果连接是关闭状态,return
  {
    LOG_WARN << "disconnected, give up writing";
    return;
  }
  // if no thing in output queue, try writing directly
  //写buffer中没有数据
  if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
  {
    //sockets::write中就是简单调用write,返回写入的字节数
    nwrote = sockets::write(channel_->fd(), data, len);
    //write成功
    if (nwrote >= 0)
    {
      //len是传入的数据长度,检查是否write完
      remaining = len - nwrote;
      //无剩余数据
      if (remaining == 0 && writeCompleteCallback_)
      {
        loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
      }
    }
    //write发生错误
    else // nwrote < 0
    {
      nwrote = 0;
      if (errno != EWOULDBLOCK)
      {
        LOG_SYSERR << "TcpConnection::sendInLoop";
        if (errno == EPIPE || errno == ECONNRESET) // FIXME: any others?
        {
          faultError = true;
        }
      }
    }
  }

  assert(remaining <= len);
  //remaining > 0有剩余数据
  if (!faultError && remaining > 0)
  {
    size_t oldLen = outputBuffer_.readableBytes();
    if (oldLen + remaining >= highWaterMark_
        && oldLen < highWaterMark_
        && highWaterMarkCallback_)
    {
      loop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
    }
    //将剩余数据放入buffer
    outputBuffer_.append(static_cast<const char*>(data)+nwrote, remaining);
    //Channel没有监听可写
    if (!channel_->isWriting())
    {
      //注册可写事件
      channel_->enableWriting();
    }
  }
}

void TcpConnection::shutdown()
{
  // FIXME: use compare and swap
  if (state_ == kConnected)//只有连接正常才能shutdown
  {
    //处于半连接,状态改变
    setState(kDisconnecting);
    // FIXME: shared_from_this()?
    //交给IO线程执行
    loop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, this));
  }
}

void TcpConnection::shutdownInLoop()
{
  loop_->assertInLoopThread();
  if (!channel_->isWriting())
  {
    // we are not writing
    //关闭写端,TcpConnection关闭连接的方式只有一种,被动关闭
    //就是对端主动关闭,本地read返回0,触发关闭逻辑
    socket_->shutdownWrite();
  }
}

// void TcpConnection::shutdownAndForceCloseAfter(double seconds)
// {
//   // FIXME: use compare and swap
//   if (state_ == kConnected)
//   {
//     setState(kDisconnecting);
//     loop_->runInLoop(std::bind(&TcpConnection::shutdownAndForceCloseInLoop, this, seconds));
//   }
// }

// void TcpConnection::shutdownAndForceCloseInLoop(double seconds)
// {
//   loop_->assertInLoopThread();
//   if (!channel_->isWriting())
//   {
//     // we are not writing
//     socket_->shutdownWrite();
//   }
//   loop_->runAfter(
//       seconds,
//       makeWeakCallback(shared_from_this(),
//                        &TcpConnection::forceCloseInLoop));
// }

void TcpConnection::forceClose()
{
  // FIXME: use compare and swap
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    setState(kDisconnecting);
    loop_->queueInLoop(std::bind(&TcpConnection::forceCloseInLoop, shared_from_this()));
  }
}

void TcpConnection::forceCloseWithDelay(double seconds)
{
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    setState(kDisconnecting);
    loop_->runAfter(
        seconds,
        makeWeakCallback(shared_from_this(),
                         &TcpConnection::forceClose));  // not forceCloseInLoop to avoid race condition
  }
}

void TcpConnection::forceCloseInLoop()
{
  loop_->assertInLoopThread();
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    // as if we received 0 byte in handleRead();
    handleClose();
  }
}

const char* TcpConnection::stateToString() const
{
  switch (state_)
  {
    case kDisconnected:
      return "kDisconnected";
    case kConnecting:
      return "kConnecting";
    case kConnected:
      return "kConnected";
    case kDisconnecting:
      return "kDisconnecting";
    default:
      return "unknown state";
  }
}

void TcpConnection::setTcpNoDelay(bool on)
{
  socket_->setTcpNoDelay(on);
}

void TcpConnection::startRead()
{
  loop_->runInLoop(std::bind(&TcpConnection::startReadInLoop, this));
}

void TcpConnection::startReadInLoop()
{
  loop_->assertInLoopThread();
  if (!reading_ || !channel_->isReading())
  {
    channel_->enableReading();
    reading_ = true;
  }
}

void TcpConnection::stopRead()
{
  loop_->runInLoop(std::bind(&TcpConnection::stopReadInLoop, this));
}

void TcpConnection::stopReadInLoop()
{
  loop_->assertInLoopThread();
  if (reading_ || channel_->isReading())
  {
    channel_->disableReading();
    reading_ = false;
  }
}

//Acceptor回调函数中调用TcpServer::newConnection
//TcpServer::newConnection中创建一个TcpConnection然后调用该函数
void TcpConnection::connectEstablished()
{
  loop_->assertInLoopThread();
  assert(state_ == kConnecting);
  setState(kConnected); //改变状态
  channel_->tie(shared_from_this());
  channel_->enableReading();//注册Channel可读事件,添加进epoll事件合集

  //TcpServer中设置的回调函数
  connectionCallback_(shared_from_this());
}

void TcpConnection::connectDestroyed()
{
  loop_->assertInLoopThread();
  if (state_ == kConnected)
  {
    //彻底关闭连接,状态改变
    setState(kDisconnected);
    channel_->disableAll();

    connectionCallback_(shared_from_this());
  }
  channel_->remove(); //从epoll事件合集中删除
}

//可读事件回调函数
void TcpConnection::handleRead(Timestamp receiveTime)
{
  loop_->assertInLoopThread();
  int savedErrno = 0;
  //从socketfd读取数据存入读buffer
  ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
  if (n > 0)
  {
    //成功读取了数据,执行回调,同样是在TcpServer中设置好的
    messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
  }
  else if (n == 0)
  {
    //客户端主动关闭了连接,
    handleClose();
  }
  else
  {
    //发生了错误
    errno = savedErrno;
    LOG_SYSERR << "TcpConnection::handleRead";
    handleError();
  }
}

//可写事件回调
void TcpConnection::handleWrite()
{
  loop_->assertInLoopThread();
  if (channel_->isWriting())
  {
    ssize_t n = sockets::write(channel_->fd(),
                               outputBuffer_.peek(),
                               outputBuffer_.readableBytes());
    if (n > 0)
    {
      //write成功,从写buffer中删除这部分数据
      outputBuffer_.retrieve(n);
      //写buffer中数据发送完了
      if (outputBuffer_.readableBytes() == 0)
      {
        //不再监听该套接字上的可写事件
        channel_->disableWriting();
        //调用用户指定的回调
        if (writeCompleteCallback_)
        {
          //在IO线程中执行
          loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
        }
        if (state_ == kDisconnecting)
        {
          shutdownInLoop();
        }
      }
    }
    else
    {
      LOG_SYSERR << "TcpConnection::handleWrite";
      // if (state_ == kDisconnecting)
      // {
      //   shutdownInLoop();
      // }
    }
  }
  else
  {
    LOG_TRACE << "Connection fd = " << channel_->fd()
              << " is down, no more writing";
  }
}

void TcpConnection::handleClose()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "fd = " << channel_->fd() << " state = " << stateToString();
  assert(state_ == kConnected || state_ == kDisconnecting);
  // we don't close fd, leave it to dtor, so we can find leaks easily.
  setState(kDisconnected);
  channel_->disableAll();

  TcpConnectionPtr guardThis(shared_from_this());
  connectionCallback_(guardThis);
  // must be the last line
  //在TcpServer中被指定为TcpServer::removeConnection
  //在其中最终执行TcpConnection::connectDestroyed,
  closeCallback_(guardThis);
}

void TcpConnection::handleError()
{
  int err = sockets::getSocketError(channel_->fd());
  LOG_ERROR << "TcpConnection::handleError [" << name_
            << "] - SO_ERROR = " << err << " " << strerror_tl(err);
}

