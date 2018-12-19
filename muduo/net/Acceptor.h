// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_ACCEPTOR_H
#define MUDUO_NET_ACCEPTOR_H
 
#include <functional>
 
#include <muduo/net/Channel.h>
#include <muduo/net/Socket.h>

namespace muduo
{
namespace net
{

class EventLoop;
class InetAddress;

///
/// Acceptor of incoming TCP connections.
///
class Acceptor : noncopyable
{
 public:
  typedef std::function<void (int sockfd, const InetAddress&)> NewConnectionCallback;

  //在TcpServer中创建时,传入loop也是传给TcpServer构造函数的loop,
  //即创建TcpServer所在的线程创建的EventLoop
  Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport);
  ~Acceptor();

  //使用该类时由用户主动调用该函数设置回调函数,在handleRead中调用
  //如果是在TCPServer中使用,TCPServer会在构造函数时设置该回调函数为TcpServer::newConnection
  void setNewConnectionCallback(const NewConnectionCallback& cb)
  { newConnectionCallback_ = cb; }

  bool listenning() const { return listenning_; }

  //开始监听
  void listen();

 private:
 //监听套接字上的回调函数,处理可读事件
  void handleRead(); 

  //所属的EventLoop所在的IO线程
  EventLoop* loop_;
  //RAII句柄,Acceptor的acceptSocket_是一个listening socket
  Socket acceptSocket_;  
  //用于观察该socket上的可读事件,回调Acceptor::handleRead(),
  //该函数调用accept接收新连接,并调用用户的回调函数
  //初始化时传入的EventLoop* 参数是传给loop_的同一个
  //即所属的那个EventLoop
  Channel acceptChannel_; 
  //设置处理新连接的回调函数
  NewConnectionCallback newConnectionCallback_;
  bool listenning_;
  int idleFd_;
}; 

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_ACCEPTOR_H
