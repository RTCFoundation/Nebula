/*******************************************************************************
 * Project:  Nebula
 * @file     SocketChannelImpl.cpp
 * @brief 
 * @author   Bwar
 * @date:    2016年8月10日
 * @note
 * Modify history:
 ******************************************************************************/
#include <cstring>
#include "codec/CodecProto.hpp"
#include "codec/CodecPrivate.hpp"
#include "codec/CodecHttp.hpp"
#include "codec/http2/CodecHttp2.hpp"
#include "codec/CodecResp.hpp"
#include "labor/Labor.hpp"
#include "labor/Manager.hpp"
#include "logger/NetLogger.hpp"
#include "SocketChannelImpl.hpp"

namespace neb
{

SocketChannelImpl::SocketChannelImpl(SocketChannel* pSocketChannel, std::shared_ptr<NetLogger> pLogger, int iFd, uint32 ulSeq, ev_tstamp dKeepAlive)
    : m_ucChannelStatus(CHANNEL_STATUS_INIT),m_eLastCodecStatus(CODEC_STATUS_OK), m_bIsClientConnection(false),
      m_iRemoteWorkerIdx(-1), m_iFd(iFd), m_uiSeq(ulSeq), m_uiForeignSeq(0), m_bPipeline(true),
      m_uiUnitTimeMsgNum(0), m_uiMsgNum(0),
      m_dActiveTime(0.0), m_dKeepAlive(dKeepAlive),
      m_pIoWatcher(NULL), m_pTimerWatcher(NULL),
      m_pRecvBuff(nullptr), m_pSendBuff(nullptr), m_pWaitForSendBuff(nullptr),
      m_pCodec(nullptr), m_pHoldingHttpMsg(nullptr), m_iErrno(0), m_pLabor(nullptr), m_pSocketChannel(pSocketChannel), m_pLogger(pLogger)
{
    memset(m_szErrBuff, 0, sizeof(m_szErrBuff));
}

SocketChannelImpl::~SocketChannelImpl()
{
    LOG4_TRACE("SocketChannelImpl::~SocketChannelImpl() fd %d, seq %u", m_iFd, m_uiSeq);
    m_listPipelineStepSeq.clear();
    if (CHANNEL_STATUS_CLOSED != m_ucChannelStatus)
    {
        Close();
    }
    FREE(m_pIoWatcher);
    FREE(m_pTimerWatcher);
    DELETE(m_pRecvBuff);
    DELETE(m_pSendBuff);
    DELETE(m_pWaitForSendBuff);
    DELETE(m_pHoldingHttpMsg);
    DELETE(m_pCodec);
}

bool SocketChannelImpl::Init(E_CODEC_TYPE eCodecType, bool bIsClient)
{
    LOG4_TRACE("fd[%d], codec_type[%d]", m_iFd, eCodecType);
    m_bIsClientConnection = bIsClient;
    try
    {
        if (m_pRecvBuff == nullptr)
        {
            m_pRecvBuff = new CBuffer();
        }
        if (m_pSendBuff == nullptr)
        {
            m_pSendBuff = new CBuffer();
        }
        if (m_pWaitForSendBuff == nullptr)
        {
            m_pWaitForSendBuff = new CBuffer();
        }
        if (m_pCodec != nullptr)
        {
            DELETE(m_pCodec);
        }
        switch (eCodecType)
        {
            case CODEC_NEBULA:
            case CODEC_PROTO:
            case CODEC_NEBULA_IN_NODE:
                m_pCodec = new CodecProto(m_pLogger, eCodecType);
                m_pCodec->SetKey(m_strKey);
                break;
            case CODEC_HTTP:
                m_pCodec = new CodecHttp(m_pLogger, eCodecType);
                m_pCodec->SetKey(m_strKey);
                break;
            case CODEC_HTTP2:
                m_pCodec = new CodecHttp2(m_pLogger, eCodecType, m_bIsClientConnection);
                ((CodecHttp2*)m_pCodec)->ConnectionSetting(m_pSendBuff);
                m_pCodec->SetKey(m_strKey);
                break;
            case CODEC_RESP:
                m_pCodec = new CodecResp(m_pLogger, eCodecType);
                m_pCodec->SetKey(m_strKey);
                break;
            case CODEC_PRIVATE:
                m_pCodec = new CodecPrivate(m_pLogger, eCodecType);
                m_pCodec->SetKey(m_strKey);
                break;
            case CODEC_UNKNOW:
                break;
            default:
                LOG4_ERROR("no codec defined for code type %d", eCodecType);
                break;
        }
    }
    catch(std::bad_alloc& e)
    {
        LOG4_ERROR("%s", e.what());
        return(false);
    }
    m_dActiveTime = m_pLabor->GetNowTime();
    return(true);
}

E_CODEC_TYPE SocketChannelImpl::GetCodecType() const
{
    return(m_pCodec->GetCodecType());
}

uint32 SocketChannelImpl::PopStepSeq(uint32 uiStreamId, E_CODEC_STATUS eCodecStatus)
{
    if (m_listPipelineStepSeq.empty())
    {
        auto iter = m_mapStreamStepSeq.find(uiStreamId);
        if (iter != m_mapStreamStepSeq.end())
        {
            uint32 uiStepSeq = iter->second;
            if (CODEC_STATUS_OK == eCodecStatus)
            {
                m_mapStreamStepSeq.erase(iter);
            }
            return(uiStepSeq);
        }
        return(0);
    }
    else
    {
        uint32 uiStepSeq = m_listPipelineStepSeq.front();
        if (CODEC_STATUS_OK == eCodecStatus)
        {
            m_listPipelineStepSeq.pop_front();
        }
        return(uiStepSeq);
    }
}

ev_tstamp SocketChannelImpl::GetKeepAlive()
{
    if (CODEC_HTTP == m_pCodec->GetCodecType())
    {
        if (((CodecHttp*)m_pCodec)->GetKeepAlive() >= 0.0)
        {
            return(((CodecHttp*)m_pCodec)->GetKeepAlive());
        }
    }
    return(m_dKeepAlive);
}

bool SocketChannelImpl::NeedAliveCheck() const
{
    if (CODEC_HTTP == m_pCodec->GetCodecType()
            || CODEC_NEBULA == m_pCodec->GetCodecType()
            || CODEC_NEBULA_IN_NODE == m_pCodec->GetCodecType())
    {
        return(false);
    }
    return(true);
}

E_CODEC_STATUS SocketChannelImpl::Send()
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[%d]", m_iFd, m_uiSeq, (int)m_ucChannelStatus);
    if (CHANNEL_STATUS_CLOSED == m_ucChannelStatus || CHANNEL_STATUS_BROKEN == m_ucChannelStatus)
    {
        LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
        return(CODEC_STATUS_EOF);
    }
    else if (CHANNEL_STATUS_ESTABLISHED != m_ucChannelStatus)
    {
        return(CODEC_STATUS_PAUSE);
    }
    if (m_pCodec == nullptr)
    {
        LOG4_ERROR("no codec found, please check whether the CODEC_TYPE is valid.");
        return(CODEC_STATUS_ERR);
    }
    int iNeedWriteLen = 0;
    iNeedWriteLen = m_pSendBuff->ReadableBytes();
    if (0 == iNeedWriteLen)
    {
        iNeedWriteLen = m_pWaitForSendBuff->ReadableBytes();
        if (0 == iNeedWriteLen)
        {
            LOG4_TRACE("no data need to send.");
            return(CODEC_STATUS_OK);
        }
        else
        {
            CBuffer* pExchangeBuff = m_pSendBuff;
            m_pSendBuff = m_pWaitForSendBuff;
            m_pWaitForSendBuff = pExchangeBuff;
            m_pWaitForSendBuff->Compact(1);
        }
    }

    m_dActiveTime = m_pLabor->GetNowTime();
    int iHadWrittenLen = 0;
    int iWrittenLen = 0;
    do
    {
        iWrittenLen = Write(m_pSendBuff, m_iErrno);
        if (iWrittenLen > 0)
        {
            iHadWrittenLen += iWrittenLen;
        }
    }
    while (iWrittenLen > 0 && iHadWrittenLen < iNeedWriteLen);
    LOG4_TRACE("iNeedWriteLen = %d, iHadWrittenLen = %d", iNeedWriteLen, iHadWrittenLen);
    if (iHadWrittenLen >= 0)
    {
        m_pLabor->IoStatAddSendBytes(m_iFd, iHadWrittenLen);
        if (m_pSendBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pSendBuff->ReadableBytes() < m_pSendBuff->Capacity() / 2))
        {
            m_pSendBuff->Compact(m_pSendBuff->ReadableBytes() * 2);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        if (iNeedWriteLen == iHadWrittenLen && 0 == m_pWaitForSendBuff->ReadableBytes())
        {
            return(CODEC_STATUS_OK);
        }
        else
        {
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
        LOG4_ERROR("send to %s[fd %d] error %d: %s", m_strIdentify.c_str(),
                m_iFd, m_iErrno, m_strErrMsg.c_str());
        m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Send(int32 iCmd, uint32 uiSeq, const MsgBody& oMsgBody)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], cmd[%u], seq[%u]", m_iFd, m_uiSeq, iCmd, uiSeq);
    if (m_pCodec == nullptr)
    {
        LOG4_ERROR("no codec found, please check whether the CODEC_TYPE is valid.");
        return(CODEC_STATUS_ERR);
    }
    E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
    int32 iMsgBodyLen = oMsgBody.ByteSize();
    MsgHead oMsgHead;
    oMsgHead.set_cmd(iCmd);
    oMsgHead.set_seq(uiSeq);
    iMsgBodyLen = (iMsgBodyLen > 0) ? iMsgBodyLen : -1;     // proto3里int赋值为0会在指定固定大小的message时有问题
    oMsgHead.set_len(iMsgBodyLen);
    switch (m_ucChannelStatus)
    {
        case CHANNEL_STATUS_ESTABLISHED:
            eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
            break;
        case CHANNEL_STATUS_CLOSED:
        case CHANNEL_STATUS_BROKEN:
            LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                    m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
            return(CODEC_STATUS_ERR);
        case CHANNEL_STATUS_TELL_WORKER:
        case CHANNEL_STATUS_WORKER:
        case CHANNEL_STATUS_TRANSFER_TO_WORKER:
        case CHANNEL_STATUS_CONNECTED:
        case CHANNEL_STATUS_TRY_CONNECT:
        case CHANNEL_STATUS_INIT:
        {
            switch (iCmd)
            {
                case CMD_RSP_TELL_WORKER:
                    m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
                    m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
                    break;
                case CMD_REQ_TELL_WORKER:
                    m_ucChannelStatus = CHANNEL_STATUS_TELL_WORKER;
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
                    break;
                case CMD_RSP_CONNECT_TO_WORKER:
                    m_ucChannelStatus = CHANNEL_STATUS_WORKER;
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
                    break;
                case CMD_REQ_CONNECT_TO_WORKER:
                    m_ucChannelStatus = CHANNEL_STATUS_TRANSFER_TO_WORKER;
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pSendBuff);
                    break;
                default:
                    eCodecStatus = m_pCodec->Encode(oMsgHead, oMsgBody, m_pWaitForSendBuff);
                    if (CODEC_STATUS_OK == eCodecStatus && (gc_uiCmdReq & iCmd))
                    {
                        eCodecStatus = CODEC_STATUS_PAUSE;
                        m_listPipelineStepSeq.push_back(uiSeq);
                    }
                    break;
            }
        }
            break;
        default:
            LOG4_ERROR("%s invalid connection status %d!", m_strIdentify.c_str(), (int)m_ucChannelStatus);
            return(CODEC_STATUS_ERR);
    }

    if (CODEC_STATUS_OK != eCodecStatus)
    {
        return(eCodecStatus);
    }

    int iNeedWriteLen = m_pSendBuff->ReadableBytes();
    if (iNeedWriteLen <= 0)
    {
        return(eCodecStatus);
    }

    errno = 0;
    int iHadWrittenLen = 0;
    int iWrittenLen = 0;
    do
    {
        iWrittenLen = Write(m_pSendBuff, m_iErrno);
        if (iWrittenLen > 0)
        {
            iHadWrittenLen += iWrittenLen;
        }
    }
    while (iWrittenLen > 0 && iHadWrittenLen < iNeedWriteLen);
    LOG4_TRACE("iNeedWriteLen = %d, iHadWrittenLen = %d", iNeedWriteLen, iHadWrittenLen);
    if (iHadWrittenLen >= 0)
    {
        m_pLabor->IoStatAddSendBytes(m_iFd, iHadWrittenLen);
        if (m_pSendBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pSendBuff->ReadableBytes() < m_pSendBuff->Capacity() / 2))
        {
            m_pSendBuff->Compact(m_pSendBuff->ReadableBytes() * 2);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        if (iNeedWriteLen == iHadWrittenLen)
        {
            if (CMD_RSP_TELL_WORKER == iCmd)
            {
                return(Send());
            }
            else
            {
                return(CODEC_STATUS_OK);
            }
        }
        else
        {
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
        LOG4_ERROR("send to %s[fd %d] error %d: %s", m_strIdentify.c_str(),
                m_iFd, m_iErrno, m_strErrMsg.c_str());
        m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Send(const HttpMsg& oHttpMsg, uint32 uiStepSeq)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[%d]", m_iFd, m_uiSeq, (int)m_ucChannelStatus);
    if (m_pCodec == nullptr)
    {
        LOG4_ERROR("no codec found, please check whether the CODEC_TYPE is valid.");
        return(CODEC_STATUS_ERR);
    }
    if ((oHttpMsg.http_major() == 2 && m_pCodec->GetCodecType() != CODEC_HTTP2)
        || (oHttpMsg.http_major() < 2 && m_pCodec->GetCodecType() != CODEC_HTTP && m_pCodec->GetCodecType() != CODEC_HTTP_CUSTOM))
    {
        LOG4_ERROR("codec type not match!");
        return(CODEC_STATUS_ERR);
    }
    E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
    switch (m_ucChannelStatus)
    {
        case CHANNEL_STATUS_ESTABLISHED:
            if (CODEC_HTTP == m_pCodec->GetCodecType())
            {
                eCodecStatus = ((CodecHttp*)m_pCodec)->Encode(oHttpMsg, m_pSendBuff);
            }
            else
            {
                eCodecStatus = ((CodecHttp2*)m_pCodec)->Encode(oHttpMsg, m_pSendBuff);
            }
            if (CODEC_STATUS_OK == eCodecStatus && uiStepSeq > 0)
            {
                if (CODEC_HTTP == m_pCodec->GetCodecType())
                {
                    m_listPipelineStepSeq.push_back(uiStepSeq);
                }
                else
                {
                    m_mapStreamStepSeq.insert(std::make_pair(((CodecHttp2*)m_pCodec)->GetLastStreamId(), uiStepSeq));
                }
            }
            break;
        case CHANNEL_STATUS_CLOSED:
        case CHANNEL_STATUS_BROKEN:
            LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                    m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
            return(CODEC_STATUS_ERR);
        case CHANNEL_STATUS_TELL_WORKER:
        case CHANNEL_STATUS_WORKER:
        case CHANNEL_STATUS_TRANSFER_TO_WORKER:
        case CHANNEL_STATUS_CONNECTED:
        case CHANNEL_STATUS_TRY_CONNECT:
        case CHANNEL_STATUS_INIT:
            if (CODEC_HTTP == m_pCodec->GetCodecType())
            {
                eCodecStatus = ((CodecHttp*)m_pCodec)->Encode(oHttpMsg, m_pWaitForSendBuff);
            }
            else
            {
                eCodecStatus = ((CodecHttp2*)m_pCodec)->Encode(oHttpMsg, m_pWaitForSendBuff);
            }
            if (CODEC_STATUS_OK == eCodecStatus && uiStepSeq > 0)
            {
                eCodecStatus = CODEC_STATUS_PAUSE;
                if (CODEC_HTTP == m_pCodec->GetCodecType())
                {
                    m_listPipelineStepSeq.push_back(uiStepSeq);
                }
                else
                {
                    m_mapStreamStepSeq.insert(std::make_pair(((CodecHttp2*)m_pCodec)->GetLastStreamId(), uiStepSeq));
                }
            }
            break;
        default:
            LOG4_ERROR("%s invalid connection status %d!", m_strIdentify.c_str(), (int)m_ucChannelStatus);
            return(CODEC_STATUS_ERR);
    }

    if (CODEC_STATUS_OK != eCodecStatus && CODEC_STATUS_PART_OK != eCodecStatus)
    {
        return(eCodecStatus);
    }

    int iNeedWriteLen = m_pSendBuff->ReadableBytes();
    if (iNeedWriteLen <= 0)
    {
        return(eCodecStatus);
    }

    int iHadWrittenLen = 0;
    int iWrittenLen = 0;
    do
    {
        iWrittenLen = Write(m_pSendBuff, m_iErrno);
        if (iWrittenLen > 0)
        {
            iHadWrittenLen += iWrittenLen;
        }
    }
    while (iWrittenLen > 0 && iHadWrittenLen < iNeedWriteLen);
    LOG4_TRACE("iNeedWriteLen = %d, iHadWrittenLen = %d", iNeedWriteLen, iHadWrittenLen);
    if (iHadWrittenLen >= 0)
    {
        m_pLabor->IoStatAddSendBytes(m_iFd, iHadWrittenLen);
        if (m_pSendBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pSendBuff->ReadableBytes() < m_pSendBuff->Capacity() / 2))
        {
            m_pSendBuff->Compact(m_pSendBuff->ReadableBytes() * 2);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        if (iNeedWriteLen == iHadWrittenLen)
        {
            if (m_pCodec->GetCodecType() == CODEC_HTTP
                    && ((CodecHttp*)m_pCodec)->GetKeepAlive() == 0.0)
            {
                return(CODEC_STATUS_EOF);
            }
            return(eCodecStatus);
        }
        else
        {
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
        LOG4_ERROR("send to %s[fd %d] error %d: %s", m_strIdentify.c_str(),
                m_iFd, m_iErrno, m_strErrMsg.c_str());
        m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Send(const RedisMsg& oRedisMsg, uint32 uiStepSeq)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[%d]", m_iFd, m_uiSeq, (int)m_ucChannelStatus);
    if (m_pCodec == nullptr)
    {
        LOG4_ERROR("no codec found, please check whether the CODEC_TYPE is valid.");
        return(CODEC_STATUS_ERR);
    }
    if (m_pCodec->GetCodecType() != CODEC_RESP)
    {
        LOG4_ERROR("codec type not match!");
        return(CODEC_STATUS_ERR);
    }
    E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
    switch (m_ucChannelStatus)
    {
        case CHANNEL_STATUS_ESTABLISHED:
            eCodecStatus = ((CodecResp*)m_pCodec)->Encode(oRedisMsg, m_pSendBuff);
            break;
        case CHANNEL_STATUS_CLOSED:
        case CHANNEL_STATUS_BROKEN:
            LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                    m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
            return(CODEC_STATUS_EOF);
        case CHANNEL_STATUS_TELL_WORKER:
        case CHANNEL_STATUS_WORKER:
        case CHANNEL_STATUS_TRANSFER_TO_WORKER:
        case CHANNEL_STATUS_CONNECTED:
        case CHANNEL_STATUS_TRY_CONNECT:
        case CHANNEL_STATUS_INIT:
            eCodecStatus = ((CodecResp*)m_pCodec)->Encode(oRedisMsg, m_pWaitForSendBuff);
            if (CODEC_STATUS_OK == eCodecStatus && uiStepSeq > 0)
            {
                eCodecStatus = CODEC_STATUS_PAUSE;
                m_listPipelineStepSeq.push_back(uiStepSeq);
            }
            break;
        default:
            LOG4_ERROR("%s invalid connection status %d!", m_strIdentify.c_str(), (int)m_ucChannelStatus);
            return(CODEC_STATUS_ERR);
    }

    if (CODEC_STATUS_OK != eCodecStatus)
    {
        return(eCodecStatus);
    }

    int iNeedWriteLen = m_pSendBuff->ReadableBytes();
    if (iNeedWriteLen <= 0)
    {
        return(eCodecStatus);
    }

    int iHadWrittenLen = 0;
    int iWrittenLen = 0;
    do
    {
        iWrittenLen = Write(m_pSendBuff, m_iErrno);
        if (iWrittenLen > 0)
        {
            iHadWrittenLen += iWrittenLen;
        }
    }
    while (iWrittenLen > 0 && iHadWrittenLen < iNeedWriteLen);
    LOG4_TRACE("iNeedWriteLen = %d, iHadWrittenLen = %d", iNeedWriteLen, iHadWrittenLen);
    if (iHadWrittenLen >= 0)
    {
        m_pLabor->IoStatAddSendBytes(m_iFd, iHadWrittenLen);
        if (m_pSendBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pSendBuff->ReadableBytes() < m_pSendBuff->Capacity() / 2))
        {
            m_pSendBuff->Compact(m_pSendBuff->ReadableBytes() * 2);
        }
        if (uiStepSeq > 0)
        {
            m_listPipelineStepSeq.push_back(uiStepSeq);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        if (iNeedWriteLen == iHadWrittenLen)
        {
            return(CODEC_STATUS_OK);
        }
        else
        {
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
        LOG4_ERROR("send to %s[fd %d] error %d: %s", m_strIdentify.c_str(),
                m_iFd, m_iErrno, m_strErrMsg.c_str());
        m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Send(const char* pRaw, uint32 uiRawSize, uint32 uiStepSeq)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[%d]", m_iFd, m_uiSeq, (int)m_ucChannelStatus);
    E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
    switch (m_ucChannelStatus)
    {
        case CHANNEL_STATUS_ESTABLISHED:
            m_pSendBuff->Write(pRaw, uiRawSize);
            break;
        case CHANNEL_STATUS_CLOSED:
        case CHANNEL_STATUS_BROKEN:
            LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                    m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
            return(CODEC_STATUS_ERR);
        case CHANNEL_STATUS_TELL_WORKER:
        case CHANNEL_STATUS_WORKER:
        case CHANNEL_STATUS_TRANSFER_TO_WORKER:
        case CHANNEL_STATUS_CONNECTED:
        case CHANNEL_STATUS_TRY_CONNECT:
        case CHANNEL_STATUS_INIT:
            m_pWaitForSendBuff->Write(pRaw, uiRawSize);
            eCodecStatus = CODEC_STATUS_PAUSE;
            break;
        default:
            LOG4_ERROR("%s invalid connection status %d!", m_strIdentify.c_str(), (int)m_ucChannelStatus);
            return(CODEC_STATUS_ERR);
    }

    if (CODEC_STATUS_OK != eCodecStatus)
    {
        return(eCodecStatus);
    }

    int iNeedWriteLen = m_pSendBuff->ReadableBytes();
    if (iNeedWriteLen <= 0)
    {
        return(eCodecStatus);
    }

    int iHadWrittenLen = 0;
    int iWrittenLen = 0;
    do
    {
        iWrittenLen = Write(m_pSendBuff, m_iErrno);
        if (iWrittenLen > 0)
        {
            iHadWrittenLen += iWrittenLen;
        }
    }
    while (iWrittenLen > 0 && iHadWrittenLen < iNeedWriteLen);
    LOG4_TRACE("iNeedWriteLen = %d, iHadWrittenLen = %d", iNeedWriteLen, iHadWrittenLen);
    if (iHadWrittenLen >= 0)
    {
        m_pLabor->IoStatAddSendBytes(m_iFd, iHadWrittenLen);
        if (m_pSendBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pSendBuff->ReadableBytes() < m_pSendBuff->Capacity() / 2))
        {
            m_pSendBuff->Compact(m_pSendBuff->ReadableBytes() * 2);
        }
        if (uiStepSeq > 0)
        {
            m_listPipelineStepSeq.push_back(uiStepSeq);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        if (iNeedWriteLen == iHadWrittenLen)
        {
            return(CODEC_STATUS_OK);
        }
        else
        {
            return(CODEC_STATUS_PAUSE);
        }
    }
    else
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            return(CODEC_STATUS_PAUSE);
        }
        m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
        LOG4_ERROR("send to %s[fd %d] error %d: %s", m_strIdentify.c_str(),
                m_iFd, m_iErrno, m_strErrMsg.c_str());
        m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
        return(CODEC_STATUS_INT);
    }
}

E_CODEC_STATUS SocketChannelImpl::Recv(MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[%d]", m_iFd, m_uiSeq, (int)m_ucChannelStatus);
    if (m_pCodec == nullptr)
    {
        LOG4_ERROR("no codec found, please check whether the CODEC_TYPE is valid.");
        return(CODEC_STATUS_ERR);
    }
    int iReadLen = 0;
    int iHadReadLen = 0;
    do
    {
        iReadLen = Read(m_pRecvBuff, m_iErrno);
        if (iReadLen > 0)
        {
            iHadReadLen += iReadLen;
        }
        LOG4_TRACE("recv from fd %d data len %d. and m_pRecvBuff->ReadableBytes() = %d",
                m_iFd, iReadLen, m_pRecvBuff->ReadableBytes());
    }
    while(iReadLen > 0);
    m_pLabor->IoStatAddRecvBytes(m_iFd, iHadReadLen);
    if (iReadLen == 0)
    {
        m_eLastCodecStatus = CODEC_STATUS_EOF;
    }
    else if (iReadLen < 0)
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            m_eLastCodecStatus = CODEC_STATUS_PAUSE;
            //return(CODEC_STATUS_PAUSE);
        }
        else
        {
            m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
            LOG4_ERROR("recv from %s[fd %d] remote %s error %d: %s",
                    m_strIdentify.c_str(), m_iFd, m_strRemoteAddr.c_str(),
                    m_iErrno, m_strErrMsg.c_str());
            m_eLastCodecStatus = CODEC_STATUS_INT;
            m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
            //return(CODEC_STATUS_INT);
        }
    }
    
    if (iHadReadLen > 0)
    {
        if (m_pRecvBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pRecvBuff->ReadableBytes() < m_pRecvBuff->Capacity() / 2))
        {
            m_pRecvBuff->Compact(m_pRecvBuff->ReadableBytes() * 2);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        E_CODEC_STATUS eCodecStatus = m_pCodec->Decode(m_pRecvBuff, oMsgHead, oMsgBody);
        if (CODEC_STATUS_OK == eCodecStatus)
        {
            switch (m_ucChannelStatus)
            {
                case CHANNEL_STATUS_ESTABLISHED:
                    if ((gc_uiCmdReq & oMsgHead.cmd()) && (m_strClientData.size() > 0))
                    {
                        m_uiForeignSeq = oMsgHead.seq();
                        ++m_uiUnitTimeMsgNum;
                        ++m_uiMsgNum;
                        oMsgBody.set_add_on(m_strClientData);
                        if (m_uiMsgNum == 1)
                        {
                            m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
                        }
                    }
                    break;
                case CHANNEL_STATUS_CLOSED:
                case CHANNEL_STATUS_BROKEN:
                    LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                            m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
                    return(CODEC_STATUS_ERR);
                case CHANNEL_STATUS_TELL_WORKER:
                case CHANNEL_STATUS_WORKER:
                case CHANNEL_STATUS_TRANSFER_TO_WORKER:
                case CHANNEL_STATUS_CONNECTED:
                case CHANNEL_STATUS_TRY_CONNECT:
                case CHANNEL_STATUS_INIT:
                {
                    switch (oMsgHead.cmd())
                    {
                        case CMD_RSP_TELL_WORKER:
                            m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
                            m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
                            break;
                        case CMD_REQ_TELL_WORKER:
                            m_ucChannelStatus = CHANNEL_STATUS_TELL_WORKER;
                            break;
                        case CMD_RSP_CONNECT_TO_WORKER:
                            m_ucChannelStatus = CHANNEL_STATUS_WORKER;
                            break;
                        case CMD_REQ_CONNECT_TO_WORKER:
                            m_ucChannelStatus = CHANNEL_STATUS_TRANSFER_TO_WORKER;
                            break;
                        default:
                            LOG4_TRACE("channel_fd[%d], channel_seq[%d], channel_status[from %d to %d] may be a fault.",
                                    m_iFd, m_uiSeq, (int)m_ucChannelStatus, CHANNEL_STATUS_ESTABLISHED);
                            m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
                            break;
                    }
                }
                    break;
                default:
                    LOG4_ERROR("%s invalid connection status %d!", m_strIdentify.c_str(), (int)m_ucChannelStatus);
                    return(CODEC_STATUS_ERR);
            }
        }
        else
        {
            if (0 == m_uiMsgNum && CODEC_STATUS_PAUSE != eCodecStatus)
            {
                return(CODEC_STATUS_INVALID);
            }
        }
        LOG4_TRACE("channel_fd[%d], channel_seq[%u], cmd[%u], seq[%u]", m_iFd, m_uiSeq, oMsgHead.cmd(), oMsgHead.seq());
        return(eCodecStatus);
    }
    return(m_eLastCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Recv(HttpMsg& oHttpMsg)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_uiSeq);
    if (CHANNEL_STATUS_CLOSED == m_ucChannelStatus || CHANNEL_STATUS_BROKEN == m_ucChannelStatus)
    {
        LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
        return(CODEC_STATUS_ERR);
    }
    if (m_pCodec == nullptr)
    {
        LOG4_ERROR("no codec found, please check whether the CODEC_TYPE is valid.");
        return(CODEC_STATUS_ERR);
    }
    int iReadLen = 0;
    int iHadReadLen = 0;
    do
    {
        iReadLen = Read(m_pRecvBuff, m_iErrno);
        LOG4_TRACE("recv from fd %d data len %d. and m_pRecvBuff->ReadableBytes() = %d",
                m_iFd, iReadLen, m_pRecvBuff->ReadableBytes());
        if (iReadLen > 0)
        {
            iHadReadLen += iReadLen;
        }
    }
    while (iReadLen > 0);
    m_pLabor->IoStatAddRecvBytes(m_iFd, iHadReadLen);

    if (iReadLen == 0)
    {
        m_eLastCodecStatus = CODEC_STATUS_EOF;
    }
    else if (iReadLen < 0)
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            m_eLastCodecStatus = CODEC_STATUS_PAUSE;
        }
        else
        {
            m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
            LOG4_ERROR("recv from %s[fd %d] remote %s error %d: %s",
                    m_strIdentify.c_str(), m_iFd, m_strRemoteAddr.c_str(),
                    m_iErrno, m_strErrMsg.c_str());
            m_eLastCodecStatus = CODEC_STATUS_INT;
            m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
        }
    }

    if (iHadReadLen > 0)
    {
        if (m_pRecvBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pRecvBuff->ReadableBytes() < m_pRecvBuff->Capacity() / 2))
        {
            m_pRecvBuff->Compact(m_pRecvBuff->ReadableBytes() * 2);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
        if (CODEC_HTTP == m_pCodec->GetCodecType())
        {
            eCodecStatus = ((CodecHttp*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg);
        }
        else
        {
            size_t uiSendBuffLen = m_pSendBuff->ReadableBytes();
            eCodecStatus = ((CodecHttp2*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg, m_pSendBuff);
            if (m_pSendBuff->ReadableBytes() > uiSendBuffLen
                    && (eCodecStatus == CODEC_STATUS_OK || eCodecStatus == CODEC_STATUS_PART_OK))
            {
                Send();
            }
        }
        if (CODEC_STATUS_OK == eCodecStatus)
        {
            ++m_uiUnitTimeMsgNum;
            ++m_uiMsgNum;
            if (m_uiMsgNum == 1)
            {
                m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
                m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
                if (oHttpMsg.has_upgrade() && oHttpMsg.upgrade().is_upgrade())
                {
                    if (std::string("h2") == oHttpMsg.upgrade().protocol()
                            || std::string("h2c") == oHttpMsg.upgrade().protocol())
                    {
                        auto h_iter = oHttpMsg.headers().find("HTTP2-Settings");
                        if (h_iter != oHttpMsg.headers().end())
                        {
                            try
                            {
                                m_pHoldingHttpMsg = new HttpMsg(oHttpMsg);
                            }
                            catch (std::bad_alloc& e)
                            {
                                LOG4_ERROR("%s", e.what());
                                return(CODEC_STATUS_ERR);
                            }
                        }
                    }
                }
            }
        }
        else
        {
            if (0 == m_uiMsgNum && CODEC_STATUS_PAUSE != eCodecStatus)
            {
                if (!m_bIsClientConnection)
                {
                    m_pSendBuff->Clear();
                }
                return(CODEC_STATUS_INVALID);
            }
        }
        if (((CodecHttp*)m_pCodec)->CloseRightAway())
        {
            eCodecStatus = CODEC_STATUS_EOF;
        }
        return(eCodecStatus);
    }
    return(m_eLastCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Recv(RedisReply& oRedisReply)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_uiSeq);
    if (CHANNEL_STATUS_CLOSED == m_ucChannelStatus || CHANNEL_STATUS_BROKEN == m_ucChannelStatus)
    {
        LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
        return(CODEC_STATUS_ERR);
    }
    if (m_pCodec == nullptr)
    {
        LOG4_ERROR("no codec found, please check whether the CODEC_TYPE is valid.");
        return(CODEC_STATUS_ERR);
    }
    int iReadLen = 0;
    int iHadReadLen = 0;
    do
    {
        iReadLen = Read(m_pRecvBuff, m_iErrno);
        LOG4_TRACE("recv from fd %d data len %d. and m_pRecvBuff->ReadableBytes() = %d",
                m_iFd, iReadLen, m_pRecvBuff->ReadableBytes());
        if (iReadLen > 0)
        {
            iHadReadLen += iReadLen;
        }
    }
    while (iReadLen > 0);
    m_pLabor->IoStatAddRecvBytes(m_iFd, iHadReadLen);

    if (iReadLen == 0)
    {
        m_eLastCodecStatus = CODEC_STATUS_EOF;
    }
    else if (iReadLen < 0)
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            m_eLastCodecStatus = CODEC_STATUS_PAUSE;
        }
        else
        {
            m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
            LOG4_ERROR("recv from %s[fd %d] remote %s error %d: %s",
                    m_strIdentify.c_str(), m_iFd, m_strRemoteAddr.c_str(),
                    m_iErrno, m_strErrMsg.c_str());
           m_eLastCodecStatus = CODEC_STATUS_INT;
           m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
        }
    }

    if (iHadReadLen > 0)
    {
        if (m_pRecvBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pRecvBuff->ReadableBytes() < m_pRecvBuff->Capacity() / 2))
        {
            m_pRecvBuff->Compact(m_pRecvBuff->ReadableBytes() * 2);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        E_CODEC_STATUS eCodecStatus = ((CodecResp*)m_pCodec)->Decode(m_pRecvBuff, oRedisReply);
        if (CODEC_STATUS_OK == eCodecStatus)
        {
            ++m_uiUnitTimeMsgNum;
            ++m_uiMsgNum;
            if (m_uiMsgNum == 1)
            {
                m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
                m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
            }
        }
        else
        {
            if (0 == m_uiMsgNum && CODEC_STATUS_PAUSE != eCodecStatus)
            {
                return(CODEC_STATUS_INVALID);
            }
        }
        return(eCodecStatus);
    }
    return(m_eLastCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Recv(CBuffer& oRawBuff)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_uiSeq);
    if (CHANNEL_STATUS_CLOSED == m_ucChannelStatus || CHANNEL_STATUS_BROKEN == m_ucChannelStatus)
    {
        LOG4_WARNING("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
        return(CODEC_STATUS_ERR);
    }
    int iReadLen = 0;
    int iHadReadLen = 0;
    do
    {
        iReadLen = Read(m_pRecvBuff, m_iErrno);
        LOG4_TRACE("recv from fd %d data len %d. and m_pRecvBuff->ReadableBytes() = %d",
                m_iFd, iReadLen, m_pRecvBuff->ReadableBytes());
        if (iReadLen > 0)
        {
            iHadReadLen += iReadLen;
        }
    }
    while (iReadLen > 0);
    m_pLabor->IoStatAddRecvBytes(m_iFd, iHadReadLen);

    if (iReadLen == 0)
    {
        m_eLastCodecStatus = CODEC_STATUS_EOF;
    }
    else if (iReadLen < 0)
    {
        if (EAGAIN == m_iErrno || EINTR == m_iErrno)    // 对非阻塞socket而言，EAGAIN不是一种错误;EINTR即errno为4，错误描述Interrupted system call，操作也应该继续。
        {
            m_dActiveTime = m_pLabor->GetNowTime();
            m_eLastCodecStatus = CODEC_STATUS_PAUSE;
        }
        else
        {
            m_strErrMsg = strerror_r(m_iErrno, m_szErrBuff, sizeof(m_szErrBuff));
            LOG4_ERROR("recv from %s[fd %d] remote %s error %d: %s",
                    m_strIdentify.c_str(), m_iFd, m_strRemoteAddr.c_str(),
                    m_iErrno, m_strErrMsg.c_str());
            m_eLastCodecStatus = CODEC_STATUS_INT;
            m_ucChannelStatus = CHANNEL_STATUS_BROKEN;
        }
    }

    if (iHadReadLen > 0)
    {
        if (m_pRecvBuff->Capacity() > CBuffer::BUFFER_MAX_READ
            && (m_pRecvBuff->ReadableBytes() < m_pRecvBuff->Capacity() / 2))
        {
            m_pRecvBuff->Compact(m_pRecvBuff->ReadableBytes() * 2);
        }
        m_dActiveTime = m_pLabor->GetNowTime();
        if (oRawBuff.Write(m_pRecvBuff, m_pRecvBuff->ReadableBytes()) > 0)
        {
            ++m_uiUnitTimeMsgNum;
            ++m_uiMsgNum;
            if (m_uiMsgNum == 1)
            {
                m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
                m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
            }
            return(CODEC_STATUS_OK);
        }
        return(CODEC_STATUS_PAUSE);
    }
    return(m_eLastCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Fetch(MsgHead& oMsgHead, MsgBody& oMsgBody)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_uiSeq);
    if (CHANNEL_STATUS_CLOSED == m_ucChannelStatus || CHANNEL_STATUS_BROKEN == m_ucChannelStatus)
    {
        LOG4_TRACE("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
        return(CODEC_STATUS_ERR);
    }
    LOG4_TRACE("fetch from fd %d and m_pRecvBuff->ReadableBytes() = %d",
            m_iFd, m_pRecvBuff->ReadableBytes());
    E_CODEC_STATUS eCodecStatus = m_pCodec->Decode(m_pRecvBuff, oMsgHead, oMsgBody);
    if (CODEC_STATUS_OK == eCodecStatus)
    {
        m_uiForeignSeq = oMsgHead.seq();
        ++m_uiUnitTimeMsgNum;
        ++m_uiMsgNum;
        if (m_uiMsgNum == 1)
        {
            m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
            m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
        }
        LOG4_TRACE("channel_fd[%d], channel_seq[%u], cmd[%u], seq[%u]", m_iFd, m_uiSeq, oMsgHead.cmd(), oMsgHead.seq());
    }
    else
    {
        if (0 == m_uiMsgNum && CODEC_STATUS_PAUSE != eCodecStatus)
        {
            return(CODEC_STATUS_INVALID);
        }
    }
    if ((CODEC_STATUS_PAUSE == eCodecStatus)
            && (CODEC_STATUS_EOF == m_eLastCodecStatus
                || CODEC_STATUS_INT == m_eLastCodecStatus))
    {
        return(m_eLastCodecStatus);
    }
    return(eCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Fetch(HttpMsg& oHttpMsg)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_uiSeq);
    if (CHANNEL_STATUS_CLOSED == m_ucChannelStatus || CHANNEL_STATUS_BROKEN == m_ucChannelStatus)
    {
        LOG4_TRACE("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
        return(CODEC_STATUS_ERR);
    }
    // 当http1.0响应包未带Content-Length头时，m_pRecvBuff可读字节数为0，以关闭连接表示数据发送完毕。
    E_CODEC_STATUS eCodecStatus = CODEC_STATUS_OK;
    if (CODEC_HTTP == m_pCodec->GetCodecType())
    {
        eCodecStatus = ((CodecHttp*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg);
    }
    else
    {
        size_t uiSendBuffLen = m_pSendBuff->ReadableBytes();
        eCodecStatus = ((CodecHttp2*)m_pCodec)->Decode(m_pRecvBuff, oHttpMsg, m_pSendBuff);
        if (m_pSendBuff->ReadableBytes() > uiSendBuffLen
                && (eCodecStatus == CODEC_STATUS_OK || eCodecStatus == CODEC_STATUS_PART_OK))
        {
            Send();
        }
    }
    if (CODEC_STATUS_OK == eCodecStatus)
    {
        ++m_uiUnitTimeMsgNum;
        ++m_uiMsgNum;
        if (m_uiMsgNum == 1)
        {
            m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
            m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
            if (oHttpMsg.has_upgrade() && oHttpMsg.upgrade().is_upgrade())
            {
                if (std::string("h2") == oHttpMsg.upgrade().protocol()
                        || std::string("h2c") == oHttpMsg.upgrade().protocol())
                {
                    auto h_iter = oHttpMsg.headers().find("HTTP2-Settings");
                    if (h_iter != oHttpMsg.headers().end())
                    {
                        try
                        {
                            m_pHoldingHttpMsg = new HttpMsg(oHttpMsg);
                        }
                        catch (std::bad_alloc& e)
                        {
                            LOG4_ERROR("%s", e.what());
                            return(CODEC_STATUS_ERR);
                        }
                    }
                }
            }
        }
    }
    else
    {
        if (0 == m_uiMsgNum && CODEC_STATUS_PAUSE != eCodecStatus)
        {
            return(CODEC_STATUS_INVALID);
        }
    }
    if ((CODEC_STATUS_PAUSE == eCodecStatus)
            && (CODEC_STATUS_EOF == m_eLastCodecStatus
                || CODEC_STATUS_INT == m_eLastCodecStatus))
    {
        return(m_eLastCodecStatus);
    }
    return(eCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Fetch(RedisReply& oRedisReply)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_uiSeq);
    if (CHANNEL_STATUS_CLOSED == m_ucChannelStatus || CHANNEL_STATUS_BROKEN == m_ucChannelStatus)
    {
        LOG4_TRACE("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
        return(CODEC_STATUS_ERR);
    }
    E_CODEC_STATUS eCodecStatus = ((CodecResp*)m_pCodec)->Decode(m_pRecvBuff, oRedisReply);
    if (CODEC_STATUS_OK == eCodecStatus)
    {
        ++m_uiUnitTimeMsgNum;
        ++m_uiMsgNum;
        if (m_uiMsgNum == 1)
        {
            m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
            m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
        }
    }
    else
    {
        if (0 == m_uiMsgNum && CODEC_STATUS_PAUSE != eCodecStatus)
        {
            return(CODEC_STATUS_INVALID);
        }
    }
    if ((CODEC_STATUS_PAUSE == eCodecStatus)
            && (CODEC_STATUS_EOF == m_eLastCodecStatus
                || CODEC_STATUS_INT == m_eLastCodecStatus))
    {
        return(m_eLastCodecStatus);
    }
    return(eCodecStatus);
}

E_CODEC_STATUS SocketChannelImpl::Fetch(CBuffer& oRawBuff)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d]", m_iFd, m_uiSeq);
    if (CHANNEL_STATUS_CLOSED == m_ucChannelStatus || CHANNEL_STATUS_BROKEN == m_ucChannelStatus)
    {
        LOG4_TRACE("%s channel_fd[%d], channel_seq[%d], channel_status[%d] remote %s EOF.",
                m_strIdentify.c_str(), m_iFd, m_uiSeq, (int)m_ucChannelStatus, m_strRemoteAddr.c_str());
        return(CODEC_STATUS_ERR);
    }
    if (oRawBuff.Write(m_pRecvBuff, m_pRecvBuff->ReadableBytes()) > 0)
    {
        ++m_uiUnitTimeMsgNum;
        ++m_uiMsgNum;
        if (m_uiMsgNum == 1)
        {
            m_dKeepAlive = m_pLabor->GetNodeInfo().dIoTimeout;
            m_ucChannelStatus = CHANNEL_STATUS_ESTABLISHED;
        }
        return(CODEC_STATUS_OK);
    }
    if (CODEC_STATUS_EOF == m_eLastCodecStatus
                || CODEC_STATUS_INT == m_eLastCodecStatus)
    {
        return(m_eLastCodecStatus);
    }
    return(CODEC_STATUS_PAUSE);
}

void SocketChannelImpl::SetSecretKey(const std::string& strKey)
{
    m_strKey = strKey;
    m_pCodec->SetKey(m_strKey);
}

void SocketChannelImpl::SetRemoteWorkerIndex(int16 iRemoteWorkerIndex)
{
    m_iRemoteWorkerIdx = iRemoteWorkerIndex;
}

Codec* SocketChannelImpl::SwitchCodec(E_CODEC_TYPE eCodecType, ev_tstamp dKeepAlive, bool bIsUpgrade)
{
    LOG4_TRACE("channel_fd[%d], channel_seq[%d], codec_type[%d], new_codec_type[%d]",
                    m_iFd, m_uiSeq, m_pCodec->GetCodecType(), eCodecType);
    if (eCodecType == m_pCodec->GetCodecType())
    {
        return(m_pCodec);
    }

    Codec* pNewCodec = NULL;
    try
    {
        switch (eCodecType)
        {
            case CODEC_NEBULA:
            case CODEC_PROTO:
            case CODEC_NEBULA_IN_NODE:
                pNewCodec = new CodecProto(m_pLogger, eCodecType);
                pNewCodec->SetKey(m_strKey);
                break;
            case CODEC_HTTP:
                pNewCodec = new CodecHttp(m_pLogger, eCodecType, dKeepAlive);
                pNewCodec->SetKey(m_strKey);
                break;
            case CODEC_HTTP2:
                pNewCodec = new CodecHttp2(m_pLogger, eCodecType, m_bIsClientConnection);
                if (bIsUpgrade)
                {
                    ((CodecHttp2*)pNewCodec)->TransferHoldingMsg(m_pHoldingHttpMsg);
                    m_pHoldingHttpMsg = nullptr;
                }
                pNewCodec->SetKey(m_strKey);
                break;
            case CODEC_RESP:
                pNewCodec = new CodecResp(m_pLogger, eCodecType);
                pNewCodec->SetKey(m_strKey);
                break;
            case CODEC_PRIVATE:
                pNewCodec = new CodecPrivate(m_pLogger, eCodecType);
                pNewCodec->SetKey(m_strKey);
                break;
            case CODEC_UNKNOW:
                break;
            default:
                LOG4_ERROR("no codec defined for code type %d", eCodecType);
                break;
        }
    }
    catch(std::bad_alloc& e)
    {
        LOG4_ERROR("%s", e.what());
        return(nullptr);
    }
    if (pNewCodec != nullptr)
    {
        DELETE(m_pCodec);
        m_pCodec = pNewCodec;
    }
    m_dKeepAlive = dKeepAlive;
    m_dActiveTime = m_pLabor->GetNowTime();
    return(m_pCodec);
}

bool SocketChannelImpl::AutoSwitchCodec()
{
    auto& vecAutoSwitchCodecType = Codec::GetAutoSwitchCodecType();
    for (auto codec_iter = vecAutoSwitchCodecType.begin();
            codec_iter != vecAutoSwitchCodecType.end(); ++codec_iter)
    {
        if (*codec_iter == GetCodecType())
        {
            m_setSkipCodecType.insert(*codec_iter);
            continue;
        }
        else
        {
            auto skip_iter = m_setSkipCodecType.find(*codec_iter);
            if (skip_iter == m_setSkipCodecType.end())
            {
                if(SwitchCodec(*codec_iter, -1.0) != nullptr)
                {
                    return(true);
                }
                return(false);
            }
            else
            {
                continue;
            }
        }
    }
    return(false);
}

ev_io* SocketChannelImpl::MutableIoWatcher()
{
    if (NULL == m_pIoWatcher)
    {
        m_pIoWatcher = (ev_io*)malloc(sizeof(ev_io));
        if (NULL != m_pIoWatcher)
        {
            memset(m_pIoWatcher, 0, sizeof(ev_io));
            m_pIoWatcher->data = m_pSocketChannel;      // (void*)(Channel*)
            m_pIoWatcher->fd = GetFd();
        }
    }
    return(m_pIoWatcher);
}

ev_timer* SocketChannelImpl::MutableTimerWatcher()
{
    if (NULL == m_pTimerWatcher)
    {
        m_pTimerWatcher = (ev_timer*)malloc(sizeof(ev_timer));
        if (NULL != m_pTimerWatcher)
        {
            memset(m_pTimerWatcher, 0, sizeof(ev_timer));
            m_pTimerWatcher->data = m_pSocketChannel;    // (void*)(Channel*)
        }
    }
    return(m_pTimerWatcher);
}

bool SocketChannelImpl::Close()
{
    LOG4_TRACE("channel[%d] channel_status %d", m_iFd, (int)m_ucChannelStatus);
    if (CHANNEL_STATUS_CLOSED != m_ucChannelStatus)
    {
        m_pSendBuff->Compact(1);
        m_pWaitForSendBuff->Compact(1);
        if (0 == close(m_iFd))
        {
            m_ucChannelStatus = CHANNEL_STATUS_CLOSED;
            LOG4_TRACE("channel[%d], channel_seq[%u] close successfully.", m_iFd, GetSequence());
            return(true);
        }
        else
        {
            LOG4_TRACE("failed to close channel(fd %d), errno %d, it will be close later.", m_iFd, errno);
            return(false);
        }
    }
    else
    {
        LOG4_TRACE("channel(fd %d, seq %u) had been closed before.", m_iFd, GetSequence());
        return(false);
    }
}

int SocketChannelImpl::Write(CBuffer* pBuff, int& iErrno)
{
    LOG4_TRACE("fd[%d], channel_seq[%u]", GetFd(), GetSequence());
    return(pBuff->WriteFD(m_iFd, iErrno));
}

int SocketChannelImpl::Read(CBuffer* pBuff, int& iErrno)
{
    LOG4_TRACE("fd[%d], channel_seq[%u]", GetFd(), GetSequence());
    return(pBuff->ReadFD(m_iFd, iErrno));
}


} /* namespace neb */
