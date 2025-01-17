/*******************************************************************************
 * Project:  Nebula
 * @file     Actor.cpp
 * @brief 
 * @author   Bwar
 * @date:    2016年8月9日
 * @note
 * Modify history:
 ******************************************************************************/

#include "actor/Actor.hpp"
#include <algorithm>
#include "ios/Dispatcher.hpp"
#include "actor/session/Session.hpp"
#include "actor/step/Step.hpp"
#include "labor/Worker.hpp"

namespace neb
{

Actor::Actor(ACTOR_TYPE eActorType, ev_tstamp dTimeout)
    : m_eActorType(eActorType),
      m_uiSequence(0), m_dActiveTime(0.0), m_dTimeout(dTimeout),
      m_pLabor(nullptr), m_pTimerWatcher(NULL), m_pContext(nullptr)
{
}

Actor::~Actor()
{
    FREE(m_pTimerWatcher);
    LOG4_TRACE("eActorType %d, seq %u, actor name \"%s\"",
            m_eActorType, GetSequence(), m_strActorName.c_str());
}

uint32 Actor::GetSequence()
{
    if ((ACT_CMD == m_eActorType || ACT_MODULE == m_eActorType) // Cmd和Module总是获取最新Seq
        || 0 == m_uiSequence)
    {
        if (nullptr != m_pLabor)
        {
            m_uiSequence = m_pLabor->GetSequence();
        }
    }
    return(m_uiSequence);
}

uint32 Actor::GetNodeId() const
{
    return(m_pLabor->GetNodeInfo().uiNodeId);
}

uint32 Actor::GetWorkerIndex() const
{
    return(((Worker*)m_pLabor)->GetWorkerInfo().iWorkerIndex);
}

const std::string& Actor::GetNodeType() const
{
    return(m_pLabor->GetNodeInfo().strNodeType);
}

const std::string& Actor::GetWorkPath() const
{
    return(m_pLabor->GetNodeInfo().strWorkPath);
}

const std::string& Actor::GetNodeIdentify() const
{
    return(m_pLabor->GetNodeInfo().strNodeIdentify);
}

const NodeInfo& Actor::GetNodeInfo() const
{
    return(m_pLabor->GetNodeInfo());
}

ev_tstamp Actor::GetDataReportInterval() const
{
    return(m_pLabor->GetNodeInfo().dDataReportInterval);
}

time_t Actor::GetNowTime() const
{
    return(m_pLabor->GetNowTime());
}

long Actor::GetNowTimeMs() const
{
    return(m_pLabor->GetNowTimeMs());
}

const CJsonObject& Actor::GetCustomConf() const
{
    return(m_pLabor->GetCustomConf());
}

std::shared_ptr<Session> Actor::GetSession(uint32 uiSessionId)
{
    auto pSession = m_pLabor->GetActorBuilder()->GetSession(uiSessionId);
    if (pSession == nullptr)
    {
        auto pActorBuilder = m_pLabor->GetLoaderActorBuilder();
        if (pActorBuilder != nullptr)
        {
            return(pActorBuilder->GetSession(uiSessionId));
        }
    }
    return(pSession);
}

std::shared_ptr<Session> Actor::GetSession(const std::string& strSessionId)
{
    auto pSession = m_pLabor->GetActorBuilder()->GetSession(strSessionId);
    if (pSession == nullptr)
    {
        auto pActorBuilder = m_pLabor->GetLoaderActorBuilder();
        if (pActorBuilder != nullptr)
        {
            return(pActorBuilder->GetSession(strSessionId));
        }
    }
    return(pSession);
}

bool Actor::ExecStep(uint32 uiStepSeq, int iErrno, const std::string& strErrMsg, void* data)
{
    return(m_pLabor->GetActorBuilder()->ExecStep(uiStepSeq, iErrno, strErrMsg, data));
}

std::shared_ptr<Operator> Actor::GetOperator(const std::string& strOperatorName)
{
    return(m_pLabor->GetActorBuilder()->GetOperator(strOperatorName));
}

std::shared_ptr<Context> Actor::GetContext()
{
    return(m_pContext);
}

void Actor::SetContext(std::shared_ptr<Context> pContext)
{
    m_pContext = pContext;
}

void Actor::AddAssemblyLine(std::shared_ptr<Session> pSession)
{
    m_pLabor->GetActorBuilder()->AddAssemblyLine(pSession);
}

bool Actor::SendTo(std::shared_ptr<SocketChannel> pChannel)
{
    return(m_pLabor->GetDispatcher()->SendTo(pChannel));
}

bool Actor::SendTo(std::shared_ptr<SocketChannel> pChannel, int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody)
{
    (const_cast<MsgBody&>(oMsgBody)).set_trace_id(GetTraceId());
    return(m_pLabor->GetDispatcher()->SendTo(pChannel, iCmd, uiSeq, oMsgBody));
}

bool Actor::SendTo(std::shared_ptr<SocketChannel> pChannel, const HttpMsg& oHttpMsg)
{
    (const_cast<HttpMsg&>(oHttpMsg)).mutable_headers()->insert({"x-trace-id", GetTraceId()});
    return(m_pLabor->GetDispatcher()->SendTo(pChannel, oHttpMsg, 0));
}

bool Actor::SendTo(std::shared_ptr<SocketChannel> pChannel, const RedisReply& oRedisReply)
{
    return(m_pLabor->GetDispatcher()->SendTo(pChannel, oRedisReply, 0));
}

bool Actor::SendTo(std::shared_ptr<SocketChannel> pChannel, const char* pRawData, uint32 uiRawDataSize)
{
    return(m_pLabor->GetDispatcher()->SendTo(pChannel, pRawData, uiRawDataSize, 0));
}

bool Actor::SendTo(const std::string& strIdentify, int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody, E_CODEC_TYPE eCodecType)
{
    (const_cast<MsgBody&>(oMsgBody)).set_trace_id(GetTraceId());
    return(m_pLabor->GetDispatcher()->SendTo(strIdentify, SOCKET_STREAM, eCodecType, false, true, iCmd, uiSeq, oMsgBody)); 
}

bool Actor::SendTo(const std::string& strHost, int iPort, const HttpMsg& oHttpMsg, uint32 uiStepSeq)
{
    bool bWithSsl = false;
    bool bPipeline = false;
    if (oHttpMsg.headers().find("x-trace-id") == oHttpMsg.headers().end())
    {
        (const_cast<HttpMsg&>(oHttpMsg)).mutable_headers()->insert({"x-trace-id", GetTraceId()});
    }
    if (oHttpMsg.http_major() >= 2)
    {
        bPipeline = true;
    }
    std::string strSchema = oHttpMsg.url().substr(0, oHttpMsg.url().find_first_of(":"));
    std::transform(strSchema.begin(), strSchema.end(), strSchema.begin(), [](unsigned char c)->unsigned char {return std::tolower(c);});
    if (strSchema == std::string("https"))
    {
        bWithSsl = true;
    }
    if (oHttpMsg.http_major() == 2)
    {
        return(m_pLabor->GetDispatcher()->SendTo(strHost, iPort, SOCKET_STREAM, CODEC_HTTP2, bWithSsl, bPipeline, oHttpMsg, GetSequence()));
    }
    else
    {
        return(m_pLabor->GetDispatcher()->SendTo(strHost, iPort, SOCKET_STREAM, CODEC_HTTP, bWithSsl, bPipeline, oHttpMsg, GetSequence()));
    }
}

bool Actor::SendTo(const std::string& strIdentify, const RedisMsg& oRedisMsg, bool bWithSsl, bool bPipeline, uint32 uiStepSeq)
{
    return(m_pLabor->GetDispatcher()->SendTo(strIdentify, SOCKET_STREAM, CODEC_RESP, bWithSsl, bPipeline, oRedisMsg, GetSequence()));
}

bool Actor::SendToCluster(const std::string& strIdentify, const RedisMsg& oRedisMsg, bool bWithSsl, bool bPipeline, bool bEnableReadOnly)
{
    return(m_pLabor->GetActorBuilder()->SendToCluster(strIdentify, bWithSsl, bPipeline, oRedisMsg, GetSequence(), bEnableReadOnly));
}

bool Actor::SendRoundRobin(const std::string& strIdentify, const RedisMsg& oRedisMsg, bool bWithSsl, bool bPipeline)
{
    return(m_pLabor->GetDispatcher()->SendRoundRobin(strIdentify, SOCKET_STREAM, CODEC_RESP, bWithSsl, bPipeline, oRedisMsg, GetSequence()));
}

bool Actor::SendTo(const std::string& strIdentify, const char* pRawData, uint32 uiRawDataSize, bool bWithSsl, bool bPipeline, uint32 uiStepSeq)
{
    return(m_pLabor->GetDispatcher()->SendTo(strIdentify, SOCKET_STREAM, CODEC_UNKNOW, bWithSsl, bPipeline, pRawData, uiRawDataSize, GetSequence()));
}

bool Actor::SendTo(int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody)
{
    (const_cast<MsgBody&>(oMsgBody)).set_trace_id(GetTraceId());
    return(m_pLabor->GetDispatcher()->SendTo(iCmd, uiSeq, oMsgBody));
}

bool Actor::SendRoundRobin(const std::string& strNodeType, int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody, E_CODEC_TYPE eCodecType)
{
    (const_cast<MsgBody&>(oMsgBody)).set_trace_id(GetTraceId());
    return(m_pLabor->GetDispatcher()->SendRoundRobin(strNodeType, SOCKET_STREAM, eCodecType, false, true, iCmd, uiSeq, oMsgBody));
}

bool Actor::SendOriented(const std::string& strNodeType, uint32 uiFactor, int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody, E_CODEC_TYPE eCodecType)
{
    (const_cast<MsgBody&>(oMsgBody)).set_trace_id(GetTraceId());
    return(m_pLabor->GetDispatcher()->SendOriented(strNodeType, SOCKET_STREAM, eCodecType, false, true, uiFactor, iCmd, uiSeq, oMsgBody));
}

bool Actor::SendOriented(const std::string& strNodeType, int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody, E_CODEC_TYPE eCodecType)
{
    (const_cast<MsgBody&>(oMsgBody)).set_trace_id(GetTraceId());
    if (oMsgBody.has_req_target())
    {
        if (0 != oMsgBody.req_target().route_id())
        {
            return(SendOriented(strNodeType, oMsgBody.req_target().route_id(), iCmd, uiSeq, oMsgBody, eCodecType));
        }
        else if (oMsgBody.req_target().route().length() > 0)
        {
            return(m_pLabor->GetDispatcher()->SendOriented(strNodeType, SOCKET_STREAM, eCodecType, false, true, oMsgBody.req_target().route(), iCmd, uiSeq, oMsgBody));
        }
        else
        {
            return(SendRoundRobin(strNodeType, iCmd, uiSeq, oMsgBody, eCodecType));
        }
    }
    else
    {
        LOG4_ERROR("the request message has no req_target.");
        return(SendRoundRobin(strNodeType, iCmd, uiSeq, oMsgBody, eCodecType));
    }
}

bool Actor::SendDataReport(int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody)
{
    (const_cast<MsgBody&>(oMsgBody)).set_trace_id(GetTraceId());
    return(m_pLabor->GetDispatcher()->SendDataReport(iCmd, uiSeq, oMsgBody));
}

void Actor::CircuitBreak(const std::string& strIdentify)
{
    m_pLabor->GetDispatcher()->CircuitBreak(strIdentify);
}

bool Actor::SendToSelf(int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody)
{
    return(m_pLabor->GetDispatcher()->SendToSelf(iCmd, uiSeq, oMsgBody, GetSequence()));
}

bool Actor::SendToSelf(const HttpMsg& oHttpMsg)
{
    return(m_pLabor->GetDispatcher()->SendToSelf(oHttpMsg, GetSequence()));
}

bool Actor::SendToSelf(const RedisMsg& oRedisMsg)
{
    return(m_pLabor->GetDispatcher()->SendToSelf(oRedisMsg, GetSequence()));
}

bool Actor::SendToSelf(const char* pRaw, uint32 uiRawSize)
{
    return(m_pLabor->GetDispatcher()->SendToSelf(pRaw, uiRawSize, GetSequence()));
}

std::shared_ptr<SocketChannel> Actor::GetLastActivityChannel()
{
    return(m_pLabor->GetDispatcher()->GetLastActivityChannel());
}

bool Actor::CloseRawChannel(std::shared_ptr<SocketChannel> pChannel)
{
    if (CODEC_UNKNOW == pChannel->GetCodecType())
    {
        m_pLabor->GetDispatcher()->Disconnect(pChannel, false);
    }
    return(false);
}

int32 Actor::GetStepNum() const
{
    return(m_pLabor->GetActorBuilder()->GetStepNum());
}

void Actor::SetLabor(Labor* pLabor)
{
    m_pLabor = pLabor;
}

uint32 Actor::ForceNewSequence()
{
    if (nullptr != m_pLabor)
    {
        m_uiSequence = m_pLabor->GetSequence();
    }
    return(m_uiSequence);
}

ev_timer* Actor::MutableTimerWatcher()
{
    if (NULL == m_pTimerWatcher)
    {
        m_pTimerWatcher = (ev_timer*)malloc(sizeof(ev_timer));
        if (NULL != m_pTimerWatcher)
        {
            memset(m_pTimerWatcher, 0, sizeof(ev_timer));
            m_pTimerWatcher->data = this;    // (void*)(Actor*)
        }
    }
    return(m_pTimerWatcher);
}

void Actor::SetActorName(const std::string& strActorName)
{
    m_strActorName = strActorName;
}

void Actor::SetTraceId(const std::string& strTraceId)
{
    m_strTraceId = strTraceId;
}

} /* namespace neb */
