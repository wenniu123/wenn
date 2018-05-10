/******************************************************************************
**
** @file
** ClientApp.cpp
** @brief
** KTV客户端，App类
** @par
** 未处理C++异常
** @author
** panxf
** @date
** 2016-9-1
**
******************************************************************************/

#include "ClientApp.h"
#include <QNetworkInterface>
#include <QDebug>
#include <SongDefine.h>
#include <QThreadPool>
#include <QFontDatabase>
#include <QFont>

#define CONFI_FileName	"ktv_client.conf"
#define CONFI_Header	"ktvClientConfig"

using namespace xgKtv;

extern void fnMsgOutput(QtMsgType, const QMessageLogContext&, const QString&);

// 构造函数
CClientApp::CClientApp(int &argc, char **argv)
	: QGuiApplication(argc, argv)
	, m_config(CONFI_FileName, CONFI_Header)
	, m_pMediaManager(NULL)
	, m_pKtvManager(NULL)
	, m_pDeviceManager(NULL)
	, m_pTvScreen(NULL)
	, m_unAccount(0)
{
	int nWaitLoop = 20;
	do { // 初始化网络信息，读取本机IP，MAC
		i_InitNetworkInfo();

		if ("" == m_strLocalIp)
		{
			SleepTime(250);
		}
		else
		{
			break;
		}
	} while (--nWaitLoop > 0);

	// 读取配置参数
	m_config.e_GetConfig("dataSourceIp", m_strDatasourceIp, "127.0.0.1");

	DEFINESZ(szUrl, 160);

	{ // 进行Https查询，提前载入OpenSsl，优化启动过程
		CHttpClient* pHttp = new CHttpClient();
		snprintf(szUrl, BUF_Len, "https://%s:%d/ktv/dbversion.html", m_strDatasourceIp.c_str(), PORT_FcgiSvcMedia);
		if (pHttp->e_HttpGet(QUrl(szUrl)))
		{
			UINT unTime = xgKtv::GetTickCount();
			do { // 进入QT事件循环
				processEvents();
				xgKtv::SleepTime(5); // 释放CPU，让DataSource尽快完成加载
			} while (pHttp->e_Status() != CHttpClient::ES_Closed && xgKtv::GetTickCount() - unTime < 8000);
		}
		delete pHttp;
	}

	{ // 查询DataSource，保证数据服务已经启动完成
		CHttpClient* pHttp = new CHttpClient();
		snprintf(szUrl, BUF_Len, "http://%s:%d/stb/query_info.html?info=pathname&file=AppResource",
				 m_strDatasourceIp.c_str(), PORT_FcgiSvcData);
		UINT unTime = xgKtv::GetTickCount();
		do {
			if (pHttp->e_HttpGet(QUrl(szUrl), 8000))
			{
				do { // 进入QT事件循环
					processEvents();
					xgKtv::SleepTime(5); // 释放CPU，让DataSource尽快完成加载
				} while (pHttp->e_Status() != CHttpClient::ES_Closed);

				QByteArray arrData = pHttp->e_GetData();
				if (!arrData.isEmpty())
				{
					const static char TAG_Result[] = "\"result\":\"";
					char* p = strstr(arrData.data(), TAG_Result);
					if (NULL != p)
					{
						p += sizeof(TAG_Result) - 1;
						char* pEnd = strchr(p, '"');
						if (NULL != pEnd)
						{
							*pEnd = '\0';
						}
						m_strAppResourcePath = p;
						break;
					}
					xgKtv::SleepTime(500);
				}
			}

		} while (xgKtv::GetTickCount() - unTime < 15000);
		delete pHttp;
	}

	if (!m_strAppResourcePath.isEmpty())
	{ // 载入字体文件
		QFontDatabase::addApplicationFont(m_strAppResourcePath + "font/arial.ttf");
		int fontId = QFontDatabase::addApplicationFont(m_strAppResourcePath + "font/weiruanyahei.ttc");
		if (fontId >= 0)
		{
			QString appFont = QFontDatabase::applicationFontFamilies(fontId).at(0);
			setFont(QFont(appFont));
		}
	}

#if defined(RELEASE_PUBLISH) && RELEASE_PUBLISH
	// 设置调试信息
	char szAccount[16] = "";
	CalcCloudAccount(m_strLocalMac.toUtf8().data(), szAccount);
	m_unAccount = atoi(szAccount);
	qInstallMessageHandler(fnMsgOutput);
#endif // RELEASE_PUBLISH
}

CClientApp::~CClientApp()
{
	SAFE_Delete(m_pMediaManager);
	SAFE_Delete(m_pKtvManager);
	SAFE_Delete(m_pDeviceManager);
	SAFE_Delete(m_pTvScreen);
}

bool CClientApp::e_Initilize()
{
	QThreadPool::globalInstance()->setMaxThreadCount(16);

	bool bRet = true;
#define ManagerNew(p, managerType) if (NULL == p) p = new managerType(this);

	ManagerNew(m_pMediaManager,  CMediaManager);
	ManagerNew(m_pKtvManager,	 CKtvManager);
	ManagerNew(m_pDeviceManager, CDeviceManager);
	ManagerNew(m_pTvScreen,		 CTvScreen);

	UINT unTime = xgKtv::GetTickCount();
	do {
		processEvents();
		xgKtv::SleepTime(5);
	} while (!m_pMediaManager->isInitOk() && !m_pKtvManager->isInitOk() && xgKtv::GetTickCount() - unTime < 6000);

	return bRet;
}

// 初始化时获取网络信息，本机IP地址
void CClientApp::i_InitNetworkInfo()
{
	QList<QNetworkInterface> listInfo = QNetworkInterface::allInterfaces();
	QList<QNetworkInterface>::iterator itor = listInfo.begin();
	for (; listInfo.end() != itor; ++itor)
	{
		QNetworkInterface& info = *itor;
	#ifdef Q_OS_WIN32
		QString strCardName = info.humanReadableName();
		if (strCardName.contains("VirtualBox") ||
			strCardName.contains("VMware") ||
			strCardName.contains("Loopback") ||
			strCardName.contains("VPN"))
		{ // 忽略掉虚拟机IP, 兼容Windows调试
			continue;
		}
	#endif

		bool bOk = false;
		QList<QNetworkAddressEntry> listEntry = info.addressEntries();
		QList<QNetworkAddressEntry>::iterator it = listEntry.begin();
		for (; listEntry.end() != it; ++it)
		{
			QNetworkAddressEntry& entry = *it;

			QHostAddress addrLocal	   = entry.ip();
			QHostAddress addrBroadcast = entry.broadcast();

			if (!addrLocal.isNull() && !addrLocal.isLoopback() && !addrBroadcast.isNull()
			  && addrLocal.protocol() == QAbstractSocket::IPv4Protocol)
			{
				m_strBroadcastIp = addrBroadcast.toString();
				m_strLocalIp	 = addrLocal.toString();
				m_strLocalMac	 = info.hardwareAddress();
				bOk = true;
				break;
			}
		}

		if (bOk)
		{
			break;
		}
	}
}

// 设置 ktv_datasource IP地址
bool CClientApp::e_SetDatasourceIp(const QString& strIp)
{
	// IP地址为本机地址时，恢复为默认地址
	std::string strDataSourceIp = strIp == m_strLocalIp ? "127.0.0.1" : strIp.toStdString();

	bool bModified = false;
	if (strDataSourceIp != m_strDatasourceIp)
	{
		m_config.e_SetConfig("dataSourceIp", strDataSourceIp.c_str());
		m_strDatasourceIp = strDataSourceIp;
		if (NULL != m_pMediaManager)
		{ // 中途改变数据源，则需要重新获取Ktv数据
			m_pMediaManager->e_GetKtvInitData();
		}
		if (NULL != m_pKtvManager)
		{ // 中途改变数据源，则需要重新获取默认歌曲/广告歌曲数据
			m_pKtvManager->e_GetCloudAccount();
		}
		bModified = true;
	}

	return bModified;
}

// 选择 系统语言
bool CClientApp::e_SelectLanguage(const QString& strLanguage)
{
	// Modified by Xiangy, 2018-02-11, 及时加载，减少内存占用
#if 1
	bool bRet = false;
	QCoreApplication::removeTranslator(&m_Translator);
	if (strLanguage.isEmpty() || "ZH" == strLanguage)
	{ // 中文(默认)
		bRet = true;
	}
	else
	{
		// "client_EN" -> 英文
		if (m_Translator.load("client_" + strLanguage, QString(":/language/")))
		{
			bRet = QCoreApplication::installTranslator(&m_Translator);
		}
	}
#else
#define SELECT_LANGUAGE(trans, lang, file) \
	if (lang == strLanguage) { \
		if (!trans.isEmpty() || trans.load(file, QString(":/language/"))) { \
			bRet = QCoreApplication::installTranslator(&trans); \
		} \
		break; \
	}

	bool bRet = false;
	switch (1) { case 1: {
		if (strLanguage.isEmpty() || "ZH" == strLanguage)
		{ // 中文(默认)
			QCoreApplication::removeTranslator(&m_TranslatorEn);
			QCoreApplication::removeTranslator(&m_TranslatorHk);
			QCoreApplication::removeTranslator(&m_TranslatorJa);
			QCoreApplication::removeTranslator(&m_TranslatorKo);
			QCoreApplication::removeTranslator(&m_TranslatorMy);
			QCoreApplication::removeTranslator(&m_TranslatorVi);
			bRet = true;
			break;
		}

		SELECT_LANGUAGE(m_TranslatorEn, "EN", "client_EN"); // 英文
		SELECT_LANGUAGE(m_TranslatorHk, "HK", "client_HK"); // 繁体
		SELECT_LANGUAGE(m_TranslatorJa, "JA", "client_JA"); // 日语
		SELECT_LANGUAGE(m_TranslatorKo, "KO", "client_KO"); // 韩语
		SELECT_LANGUAGE(m_TranslatorMy, "MY", "client_MY"); // 缅甸语
		SELECT_LANGUAGE(m_TranslatorVi, "VI", "client_VI"); // 越南语
	}} // switch (1)
#endif //

	return bRet;
}

// 获取毛玻璃图像
bool CClientApp::e_GetGlassImage(const QString& strFileName, QImage& image)
{
	bool bRet = false;
	if (m_GlassImage.isNull() || m_strGlassFileName != strFileName)
	{
		bRet = m_GlassImage.load(":/images/主页/" + strFileName);
		m_strGlassFileName = strFileName;
	}

	image = m_GlassImage;
	return bRet;
}

// 保存配置
void CClientApp::e_SaveConfig()
{
	if (m_config.e_SaveConfig())
	{ // 保存机顶盒配置到DataSource，再上传到云端
		e_GetDeviceManager().e_SaveStbSettings();
	}
}
