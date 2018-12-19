// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/TcpServer.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Acceptor.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThreadPool.h>
#include <muduo/net/SocketsOps.h>

#include <stdio.h>  // snprintf

using namespace muduo;
using namespace muduo::net;

TcpServer::TcpServer(EventLoop* loop,
                     const InetAddress& listenAddr, //一个有给定IP和port创建的InetAddress对象
                     const string& nameArg,   //用户给定的一个string类的服务器名字
                     Option option)
  : loop_(CHECK_NOTNULL(loop)),
    ipPort_(listenAddr.toIpPort()),
    name_(nameArg),
    acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),  //new一个Acceptor对象
    threadPool_(new EventLoopThreadPool(loop, name_)),
    connectionCallback_(defaultConnectionCallback),
    messageCallback_(defaultMessageCallback),
    nextConnId_(1) //序列号,用来给每一个connections_生成不同的key
{
  //将Acceptor的可读回调函数设置为TcpServer::newConnection
  acceptor_->setNewConnectionCallback(
      std::bind(&TcpServer::newConnection, this, _1, _2));
}

//析构时关闭所有连接
TcpServer::~TcpServer()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "TcpServer::~TcpServer [" << name_ << "] destructing";

  for (auto& item : connections_)
  {
    TcpConnectionPtr conn(item.second);   //map里second是TcpConnectionPtr类
    item.second.reset();
    conn->getLoop()->runInLoop(
      std::bind(&TcpConnection::connectDestroyed, conn));//关闭连接
  }
}

//设置线程池的线程数目
void TcpServer::setThreadNum(int numThreads)
{
  assert(0 <= numThreads);
  threadPool_->setThreadNum(numThreads);
}

//核心函数
void TcpServer::start()
{
  if (started_.getAndSet(1) == 0)
  {
    //启动线程池创建IO线程
    //IO线程执行EventLoop::loop,开始等待事件的发生
    //
    threadPool_->start(threadInitCallback_);

    //确保还没有开始监听
    assert(!acceptor_->listenning());

    ///开始监听,loop_是创建TCPServer所在线程传进来的EventLoop
    //执行EventLoop:::runInLoop执行任务,接收新连接
    loop_->runInLoop(
        std::bind(&Acceptor::listen, get_pointer(acceptor_)));
  }
}
 
//Acceptor调用的回调函数,主要用来创建一个TCPConnection对象,
///并将其加入ConnectionMap,设置好callback
//再调用conn->connectEstablished()
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
  loop_->assertInLoopThread();
  //从线程池取一个EventLoop对象,位于栈上
  EventLoop* ioLoop = threadPool_->getNextLoop();
  char buf[64];
  snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
  //序列号+1
  ++nextConnId_; 
  string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_
           << "] - new connection [" << connName
           << "] from " << peerAddr.toIpPort();
  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  //创建指向TcpConnection类型的智能指针
  TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                          connName,
                                          sockfd,
                                          localAddr,
                                          peerAddr));
  //connections_是map类
  //key是服务器名称(用户给的string)name_,和一个int组成 
  connections_[connName] = conn;
  //设置TcpConncetion的一系列回调函数
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  //TCP连接关闭时的回调函数
  conn->setCloseCallback(
      std::bind(&TcpServer::removeConnection, this, _1)); // FIXME: unsafe
  //在IO线程中执行建立连接的过程
  ioLoop->runInLoop(std::bind(&TcpConnection::connectEstablished, conn));
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
  // FIXME: unsafe
  loop_->runInLoop(std::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
           << "] - connection " << conn->name();
  size_t n = connections_.erase(conn->name());
  (void)n;
  assert(n == 1);
  EventLoop* ioLoop = conn->getLoop();
  ioLoop->queueInLoop(
      std::bind(&TcpConnection::connectDestroyed, conn));
}

