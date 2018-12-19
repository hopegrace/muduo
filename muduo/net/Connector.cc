// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/net/Connector.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Channel.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/SocketsOps.h>

#include <errno.h>

using namespace muduo;
using namespace muduo::net;
 
const int Connector::kMaxRetryDelayMs;

Connector::Connector(EventLoop* loop, const InetAddress& serverAddr)
  : loop_(loop),
    serverAddr_(serverAddr),
    connect_(false),
    state_(kDisconnected),
    retryDelayMs_(kInitRetryDelayMs)
{
  LOG_DEBUG << "ctor[" << this << "]";
}

Connector::~Connector()
{
  LOG_DEBUG << "dtor[" << this << "]";
  assert(!channel_);
}

void Connector::start()
{
  connect_ = true;
  //在所属IO线程Lloop中调用该函数
  loop_->runInLoop(std::bind(&Connector::startInLoop, this)); // FIXME: unsafe
}


void Connector::startInLoop()
{
  loop_->assertInLoopThread();
  assert(state_ == kDisconnected);
  if (connect_) //在start中会将其设置为true
  {
    connect();  //请求连接
  }
  else
  {
    LOG_DEBUG << "do not connect";
  }
}

void Connector::stop()
{
  connect_ = false;
  loop_->queueInLoop(std::bind(&Connector::stopInLoop, this)); // FIXME: unsafe
  // FIXME: cancel timer
}

void Connector::stopInLoop()
{
  loop_->assertInLoopThread();
  if (state_ == kConnecting)
  {
    setState(kDisconnected);
    int sockfd = removeAndResetChannel();
    retry(sockfd);
  }
}

void Connector::connect()
{
  //使用socket选项SOCK_NONBLOCK创建一个非阻塞socket
  int sockfd = sockets::createNonblockingOrDie(serverAddr_.family());
  //调用connect进行请求连接
  int ret = sockets::connect(sockfd, serverAddr_.getSockAddr());
  int savedErrno = (ret == 0) ? 0 : errno;
  switch (savedErrno)
  {
    case 0:
    case EINPROGRESS: //socket是非阻塞,返回这个错误码表示正在连接
    case EINTR:
    case EISCONN: //连接成功
      connecting(sockfd);
      break;

    case EAGAIN:  //connect返回EAGAIN是真的错误了
    case EADDRINUSE:
    case EADDRNOTAVAIL:
    case ECONNREFUSED:
    case ENETUNREACH:
      retry(sockfd);  //重连
      break;

    case EACCES:
    case EPERM:
    case EAFNOSUPPORT:
    case EALREADY:
    case EBADF:
    case EFAULT:
    case ENOTSOCK:
      LOG_SYSERR << "connect error in Connector::startInLoop " << savedErrno;
      sockets::close(sockfd); //关闭socket
      break;

    default:
      LOG_SYSERR << "Unexpected error in Connector::startInLoop " << savedErrno;
      sockets::close(sockfd);
      // connectErrorCallback_();
      break;
  }
}

void Connector::restart()
{
  loop_->assertInLoopThread();
  setState(kDisconnected);
  retryDelayMs_ = kInitRetryDelayMs;
  connect_ = true;
  startInLoop();
}

void Connector::connecting(int sockfd)
{
  //设置状态为正在进行连接
  setState(kConnecting);
  assert(!channel_);
  //将该channel和EventLoop,socket关联
  //channel对应于一个文件描述符,所以在有了socket后才能创建channel
  channel_.reset(new Channel(loop_, sockfd));
  //设置回调函数
  channel_->setWriteCallback(
      std::bind(&Connector::handleWrite, this)); // FIXME: unsafe
  channel_->setErrorCallback(
      std::bind(&Connector::handleError, this)); // FIXME: unsafe

  // channel_->tie(shared_from_this()); is not working,
  // as channel_ is not managed by shared_ptr
  //注册可写事件
  channel_->enableWriting();
}

int Connector::removeAndResetChannel()
{
  channel_->disableAll();
  channel_->remove();
  int sockfd = channel_->fd();
  // Can't reset channel_ here, because we are inside Channel::handleEvent
  loop_->queueInLoop(std::bind(&Connector::resetChannel, this)); // FIXME: unsafe
  return sockfd;
}

void Connector::resetChannel()
{
  channel_.reset();
}

//可写事件的回调函数
void Connector::handleWrite()
{
  LOG_TRACE << "Connector::handleWrite " << state_;

  if (state_ == kConnecting)
  {
    //移除channel(Connector的channel只管理建立连接的阶段),成功建立连接后
    //交给TcpClient的TcpConnection来管理
    int sockfd = removeAndResetChannel();
    //可写并不一定连接建立成功
    //如果连接发生错误,socket会是可读可写的
    //所以还需要调用getsockopt检查是否出错
    int err = sockets::getSocketError(sockfd);
    if (err)
    {
      LOG_WARN << "Connector::handleWrite - SO_ERROR = "
               << err << " " << strerror_tl(err);
      retry(sockfd);  //出错重连
    }
    else if (sockets::isSelfConnect(sockfd))  //是否是自连接
    {
      LOG_WARN << "Connector::handleWrite - Self connect";
      retry(sockfd);  //自连接的话断开重连
    }
    else
    {
      //连接成功建立,更改状态
      //调用TcpClient设置的回调函数,创建TcpConnection对象
      setState(kConnected);
      if (connect_)
      {
        newConnectionCallback_(sockfd);
      }
      else
      {
        sockets::close(sockfd);
      }
    }
  }
  else
  {
    // what happened?
    assert(state_ == kDisconnected);
  }
}

void Connector::handleError()
{
  LOG_ERROR << "Connector::handleError state=" << state_;
  if (state_ == kConnecting)
  {
    int sockfd = removeAndResetChannel();
    int err = sockets::getSocketError(sockfd);
    LOG_TRACE << "SO_ERROR = " << err << " " << strerror_tl(err);
    retry(sockfd);
  }
}

void Connector::retry(int sockfd)
{
  //socket是一次性的,失败后需要关闭重新创建
  sockets::close(sockfd);
  setState(kDisconnected);
  if (connect_)
  {
    LOG_INFO << "Connector::retry - Retry connecting to " << serverAddr_.toIpPort()
             << " in " << retryDelayMs_ << " milliseconds. ";
    //隔一段时间后重连,重新执行startInLoop
    loop_->runAfter(retryDelayMs_/1000.0,
                    std::bind(&Connector::startInLoop, shared_from_this()));
    //间隔时间翻倍
    retryDelayMs_ = std::min(retryDelayMs_ * 2, kMaxRetryDelayMs);
  }
  else
  {
    LOG_DEBUG << "do not connect";
  }
}

