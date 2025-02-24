//
//  Thread.hpp
//  Threads
//
//  Created by Gusts Kaksis on 21/11/2020.
//  Copyright © 2020 Gusts Kaksis. All rights reserved.
//

#ifndef Thread_hpp
#define Thread_hpp

#include <thread>
#include <atomic>
#include <chrono>
#include <queue>
#include <map>
#include <mutex>
#include <utility>
#include <future>

namespace
{
constexpr const std::size_t MaxSpinCycles { 1000 };
}

namespace gusc::Threads
{

/// @brief Class representing a new thread
class Thread
{
public:
    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;
    virtual ~Thread()
    {
        setIsAcceptingMessages(false);
        setIsRunning(false);
        queueWait.notify_one();
        join();
    }
    
    /// @brief start the thread and it's run-loop
    virtual void start()
    {
        if (!getIsRunning())
        {
            setIsRunning(true);
            thread = std::make_unique<std::thread>(&Thread::runLoop, this);
        }
        else
        {
            throw std::runtime_error("Thread already started");
        }
    }
    
    /// @brief signal the thread to stop - this also stops receiving messages
    /// @warning if a message is sent after calling this method an exception will be thrown
    virtual void stop()
    {
        if (thread)
        {
            setIsAcceptingMessages(false);
            setIsRunning(false);
            queueWait.notify_one();
        }
        else
        {
            throw std::runtime_error("Thread has not been started");
        }
    }
    
    /// @brief join the thread and wait unti it's finished
    void join()
    {
        if (thread && thread->joinable())
        {
            thread->join();
        }
    }
    
    /// @brief send a message that needs to be executed on this thread
    /// @param newMessage - any callable object that will be executed on this thread
    template<typename TCallable>
    void send(const TCallable& newMessage)
    {
        if (getIsAcceptingMessages())
        {
            std::lock_guard<std::mutex> lock(messageMutex);
            messageQueue.emplace(std::make_unique<CallableMessage<TCallable>>(newMessage));
            queueWait.notify_one();
        }
        else
        {
            throw std::runtime_error("Thread is not excepting any messages, the thread has been signaled for stopping");
        }
    }
    
    /// @brief send a delayed message that needs to be executed on this thread
    /// @param newMessage - any callable object that will be executed on this thread
    template<typename TCallable>
    void sendDelayed(const TCallable& newMessage, const std::chrono::milliseconds& timeout)
    {
        if (getIsAcceptingMessages())
        {
            std::lock_guard<std::mutex> lock(messageMutex);
            auto time = std::chrono::steady_clock::now() + timeout;
            delayedQueue.emplace(time, std::make_unique<CallableMessage<TCallable>>(newMessage));
            queueWait.notify_one();
        }
        else
        {
            throw std::runtime_error("Thread is not excepting any messages, the thread has been signaled for stopping");
        }
    }
    
    /// @brief send an asynchronous message that returns value and needs to be executed on this thread (calling thread is not blocked)
    /// @note if sent from the same thread this method will call the callable immediatelly to prevent deadlocking
    /// @param newMessage - any callable object that will be executed on this thread and it must return a value of type specified in TReturn (signature: TReturn(void))
    template<typename TReturn, typename TCallable>
    std::future<TReturn> sendAsync(const TCallable& newMessage)
    {
        if (getIsAcceptingMessages())
        {
            std::promise<TReturn> promise;
            std::future<TReturn> future = promise.get_future();
            if (getIsSameThread())
            {
                // If we are on the same thread excute message immediatelly to prent a deadlock
                auto message = std::make_unique<CallableMessageWithPromise<TReturn, TCallable>>(newMessage, std::move(promise));
                message->call();
            }
            else
            {
                std::lock_guard<std::mutex> lock(messageMutex);
                messageQueue.emplace(std::make_unique<CallableMessageWithPromise<TReturn, TCallable>>(newMessage, std::move(promise)));
                queueWait.notify_one();
            }
            return future;
        }
        else
        {
            throw std::runtime_error("Thread is not excepting any messages, the thread has been signaled for stopping");
        }
    }
    
    /// @brief send a synchronous message that returns value and needs to be executed on this thread (calling thread is blocked until message returns)
    /// @note to prevent deadlocking this method throws exception if called before thread has started
    /// @param newMessage - any callable object that will be executed on this thread and it must return a value of type specified in TReturn (signature: TReturn(void))
    template<typename TReturn, typename TCallable>
    TReturn sendSync(const TCallable& newMessage)
    {
        if (!getIsRunning())
        {
            throw std::runtime_error("Can not place a blocking message if the thread is not started");
        }
        auto future = sendAsync<TReturn>(newMessage);
        return future.get();
    }
    
    /// @brief send a message that needs to be executed on this thread and wait for it's completion
    /// @param newMessage - any callable object that will be executed on this thread
    template<typename TCallable>
    void sendWait(const TCallable& newMessage)
    {
        sendSync<void>(newMessage);
    }
        
    inline bool operator==(const Thread& other) const noexcept
    {
        return getId() == other.getId();
    }
    inline bool operator!=(const Thread& other) const noexcept
    {
        return !(operator==(other));
    }
    inline bool operator==(const std::thread::id& other) const noexcept
    {
        return getId() == other;
    }
    inline bool operator!=(const std::thread::id& other) const noexcept
    {
        return !(operator==(other));
    }
    
protected:
    void runLoop()
    {
        while (getIsRunning())
        {
            std::unique_ptr<Message> next;
            {
                const auto timeNow = std::chrono::steady_clock::now();
                std::unique_lock<std::mutex> lock(messageMutex);
                // Wait for any messages to arrive
                if (messageQueue.empty() && delayedQueue.empty())
                {
                    // We wait for a new message to be pushed on any of the queues
                    queueWait.wait(lock);
                }
                // Move delayed messages to main queue
                if (!delayedQueue.empty())
                {
                    auto hasNew { false };
                    for (auto it = delayedQueue.begin(); it != delayedQueue.end();)
                    {
                        if (it->first < timeNow)
                        {
                            messageQueue.emplace(std::move(it->second));
                            it = delayedQueue.erase(it);
                            hasNew = true;
                        }
                        else
                        {
                            break;
                        }
                    }
                    if (!hasNew)
                    {
                        // If there are queued items but none were added to the queue wait till next queued item
                        auto nextTime = delayedQueue.lower_bound(timeNow);
                        if (nextTime != delayedQueue.end())
                        {
                            queueWait.wait_until(lock, nextTime->first);
                        }
                    }
                }
                // Get the next message from the main queue
                if (!messageQueue.empty())
                {
                    next = std::move(messageQueue.front());
                    messageQueue.pop();
                }
            }
            if (next)
            {
                missCounter = 0;
                next->call();
            }
        }
        runLeftovers();
    }
    
    void runLeftovers()
    {
        // Process any leftover messages
        std::lock_guard<std::mutex> lock(messageMutex);
        while (messageQueue.size())
        {
            messageQueue.front()->call();
            messageQueue.pop();
        }
    }
    
    inline std::thread::id getId() const noexcept
    {
        if (thread)
        {
            return thread->get_id();
        }
        else
        {
            // We haven't started a thread yet, so we're the same thread
            return std::this_thread::get_id();
        }
    }
    
    inline bool getIsRunning() const noexcept
    {
        return isRunning;
    }

    inline void setIsRunning(bool newIsRunning) noexcept
    {
        isRunning = newIsRunning;
    }
    
    inline bool getIsAcceptingMessages() const noexcept
    {
        return isAcceptingMessages;
    }

    inline void setIsAcceptingMessages(bool newIsAcceptingMessages) noexcept
    {
        isAcceptingMessages = newIsAcceptingMessages;
    }
    
    inline bool getIsSameThread() const noexcept
    {
        return getId() == std::this_thread::get_id();
    }

private:
    
    /// @brief base class for thread message
    class Message
    {
    public:
        virtual ~Message() = default;
        virtual void call() {}
    };
    
    /// @brief templated message to wrap a callable object
    template<typename TCallable>
    class CallableMessage : public Message
    {
    public:
        CallableMessage(const TCallable& initCallableObject)
            : callableObject(initCallableObject)
        {}
        void call() override
        {
            callableObject();
        }
    private:
        TCallable callableObject;
    };
    
    /// @brief templated message to wrap a callable object which accepts promise object that can be used to signal finish of the callable (useful for subsequent async calls)
    template<typename TReturn, typename TCallable>
    class CallableMessageWithPromise : public Message
    {
    public:
        CallableMessageWithPromise(const TCallable& initCallableObject, std::promise<TReturn> initWaitablePromise)
            : callableObject(initCallableObject)
            , waitablePromise(std::move(initWaitablePromise))
        {}
        void call() override
        {
            actualCall<TReturn>();
        }
    private:
        TCallable callableObject;
        std::promise<TReturn> waitablePromise;
        
        template<typename TR>
        void actualCall()
        {
            waitablePromise.set_value(callableObject());
        }
        
        template<>
        void actualCall<void>()
        {
            callableObject();
            waitablePromise.set_value();
        }
        
    };
    
    std::size_t missCounter { 0 };
    std::atomic<bool> isRunning { false };
    std::atomic<bool> isAcceptingMessages { true };
    std::queue<std::unique_ptr<Message>> messageQueue;
    std::map<std::chrono::time_point<std::chrono::steady_clock>, std::unique_ptr<Message>> delayedQueue;
    std::unique_ptr<std::thread> thread;
    std::condition_variable queueWait;
    std::mutex messageMutex;
};

/// @brief Class representing a currently executing thread
class ThisThread : public Thread
{
public:
    ThisThread()
    {
        // ThisThread is already running
        setIsRunning(true);
    }
    
    /// @brief start the thread and it's run-loop
    /// @warning calling this method will efectivelly block current thread
    void start() override
    {
        runLoop();
    }

    /// @brief signal the thread to stop - this also stops receiving messages
    /// @warning if a message is sent after calling this method an exception will be thrown
    void stop() override
    {
        setIsAcceptingMessages(false);
        setIsRunning(false);
    }
};
    
}

#endif /* Thread_hpp */
