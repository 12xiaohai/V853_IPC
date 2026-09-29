#ifndef _MESSAGEMANAGER_H_
#define _MESSAGEMANAGER_H_

#include <vector>
#include <list>
#include <condition_variable>

namespace EyeseeLinux {

class BaseMsgContent
{
public:
    virtual ~BaseMsgContent();
};

class SmartMessage
{
public:
    /**
      User-defined message code so that the recipient can identify 
      what this message is about.
    */
    int what;
    std::vector<int> args;
    std::shared_ptr<BaseMsgContent> spData;
    class MessageReply
    {
    public:
        int mnReplyResult;
        BaseMsgContent *mpReplyData; //derived class type is based on what.

        void NotifyOne();
        int WaitReply(int nTimeout); //unit:ms
        MessageReply();
        ~MessageReply();
    private:
        /**
          @param mReplyMutex, mReplyCV, mnReplySemValue
            these params are used to implement semaphore.
        */
        std::mutex mReplyMutex;
        std::condition_variable mReplyCV;
        int mnReplySemValue;
    };
    std::shared_ptr<MessageReply> spMsgReply;

    int Reset();

    SmartMessage& operator=(const SmartMessage& lRef); //copy assignment
    SmartMessage& operator=(SmartMessage&& rRef); //move assignment, param rRef can't be const, because we will change rRef's member.
    SmartMessage();
    SmartMessage(const SmartMessage& lRef); //copy constructor
    SmartMessage(SmartMessage&& rRef); //move constructor
    ~SmartMessage();
};

class MessageManager
{
public:
    int PutMessage(const SmartMessage& MsgIn);
    int GetMessage(SmartMessage& MsgOut);   //return value is NOT_ENOUGH_DATA, UNKNOWN_ERROR, NO_ERROR
    int WaitMessage(unsigned int timeout = 0);  //return value is the message number.
    void ClearMessageList();
    int GetMessageCount();
    MessageManager(int nMsgCount=8);
    ~MessageManager();
private:
    std::mutex mLock;
    std::condition_variable mcvMsgListChanged;
    bool mWaitMsgFlag;
    int mCount; //total message count, including idle and valid.
    std::list<SmartMessage>  mValidMsgList;
    std::list<SmartMessage>  mIdleMsgList;
};

}
#endif  /* _MESSAGEMANAGER_H_ */

