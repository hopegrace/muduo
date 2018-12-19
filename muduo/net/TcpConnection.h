// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_TCPCONNECTION_H
#define MUDUO_NET_TCPCONNECTION_H

#include <muduo/base/noncopyable.h>
#include <muduo/base/StringPiece.h>
#include <muduo/base/Types.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/Buffer.h>
#include <muduo/net/InetAddress.h>

#include <memory>

#include <boost/any.hpp>

// struct tcp_info is in <netinet/tcp.h>
struct tcp_info;

namespace muduo
{
namespace net
{

class Channel;
class EventLoop;
class Socket;

///
/// TCP connection, for both client and server usage.
///
/// This is an interface class, so don't expose too much details.

//TcpConnection使用shared_ptr来管理
//在TcpServer中的map(connections_)中持有它的智能指针
//TcpConnection中的一系列回调函数都是在TcpServer中创建时设置好的
//而TcpServer中的这些回调函数是用户主动设置的,所以还是需要用户显示设置好各类回调函数
class TcpConnection : noncopyable,
                      public std::enable_shared_from_this<TcpConnection>
{
 public:
  /// Constructs a TcpConnection with a connected sockfd
  ///
  /// User should not create this object.
  TcpConnection(EventLoop* loop,
                const string& name,
                int sockfd,
                const InetAddress& localAddr,
                const InetAddress& peerAddr);
  ~TcpConnection();

  EventLoop* getLoop() const { return loop_; }
  const string& name() const { return name_; }
  const InetAddress& localAddress() const { return localAddr_; }
  const InetAddress& peerAddress() const { return peerAddr_; }
  bool connected() const { return state_ == kConnected; }
  bool disconnected() const { return state_ == kDisconnected; }
  // return true if success.
  bool getTcpInfo(struct tcp_info*) const;
  string getTcpInfoString() const;

  // void send(string&& message); // C++11
  void send(const void* message, int len);
  void send(const StringPiece& message);
  // void send(Buffer&& message); // C++11
  void send(Buffer* message);  // this one will swap data
  void shutdown(); // NOT thread safe, no simultaneous calling
  // void shutdownAndForceCloseAfter(double seconds); // NOT thread safe, no simultaneous calling
  void forceClose();
  void forceCloseWithDelay(double seconds);
  void setTcpNoDelay(bool on);
  // reading or not
  void startRead();
  void stopRead();
  bool isReading() const { return reading_; }; // NOT thread safe, may race with start/stopReadInLoop

  void setContext(const boost::any& context)
  { context_ = context; }

  const boost::any& getContext() const
  { return context_; }

  boost::any* getMutableContext()
  { return &context_; }

  void setConnectionCallback(const ConnectionCallback& cb)
  { connectionCallback_ = cb; }

  void setMessageCallback(const MessageCallback& cb)
  { messageCallback_ = cb; }

  void setWriteCompleteCallback(const WriteCompleteCallback& cb)
  { writeCompleteCallback_ = cb; }

  void setHighWaterMarkCallback(const HighWaterMarkCallback& cb, size_t highWaterMark)
  { highWaterMarkCallback_ = cb; highWaterMark_ = highWaterMark; }

  /// Advanced interface
  Buffer* inputBuffer()
  { return &inputBuffer_; }

  Buffer* outputBuffer()
  { return &outputBuffer_; }

  /// Internal use only.
  void setCloseCallback(const CloseCallback& cb)
  { closeCallback_ = cb; }

  // called when TcpServer accepts a new connection
  void connectEstablished();   // should be called only once
  // called when TcpServer has removed me from its map
  void connectDestroyed();  // should be called only once

 private:
  //定义枚举类型StateE,有这四种状态
  //构造函数中state_成员被声明为kConnecting:正在连接
  //TcpServer中创建完成后调用TcpConnection::connectEstablished将状态设置为kConnected
  enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };
  void handleRead(Timestamp receiveTime);
  void handleWrite();
  void handleClose();
  void handleError();
  // void sendInLoop(string&& message);
  void sendInLoop(const StringPiece& message);
  void sendInLoop(const void* message, size_t len);
  void shutdownInLoop();
  // void shutdownAndForceCloseInLoop(double seconds);
  void forceCloseInLoop();
  void setState(StateE s) { state_ = s; }
  const char* stateToString() const;
  void startReadInLoop();
  void stopReadInLoop();

  //所属EventLoop,也就是在TCPServer接收新连接时调用TcpServer::newConnection
  //在其中从EventLoopThreadPool拿一个线程对应的EventLoop
  //也就是说每一个TcpConnecction对应于一个IO线程
  EventLoop* loop_;
  const string name_; //连接名,在TCPServer中创建时传入
  StateE state_;  // FIXME: use atomic variable
  bool reading_;
  // we don't expose those classes to client.
  std::unique_ptr<Socket> socket_;
  //TcpConnection通过Channel获得socket上的事件,在构造函数中初始化
  //用传入的EventLoop初始化
  std::unique_ptr<Channel> channel_;
  const InetAddress localAddr_;
  //TCP连接对端的信息,是从Acceptor接收新连接时accept返回的
  const InetAddress peerAddr_;
  ConnectionCallback connectionCallback_;
  //消息到达时的回调函数,需要用户自定义,如果该类是在TcpServer中
  //用户也需要显示调用TcpServer::setMessageCallback来设置回调函数
  //设置完成后,会在TcpServer::newConnection中构造TcpConnection实例时赋给它
  MessageCallback messageCallback_;   
  WriteCompleteCallback writeCompleteCallback_;
  HighWaterMarkCallback highWaterMarkCallback_;
  CloseCallback closeCallback_;
  size_t highWaterMark_;
  //读buffer
  Buffer inputBuffer_;
  //写buffer
  Buffer outputBuffer_; // FIXME: use list<Buffer> as output buffer.
  boost::any context_;
  // FIXME: creationTime_, lastReceiveTime_
  //        bytesReceived_, bytesSent_
};

typedef std::shared_ptr<TcpConnection> TcpConnectionPtr;

}  // namespace net
}  // namespace muduo

#endif  // MUDUO_NET_TCPCONNECTION_H

