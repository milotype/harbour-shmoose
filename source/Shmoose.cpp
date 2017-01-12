#include "Shmoose.h"

#include <iostream>
#include <boost/bind.hpp>
#include <boost/smart_ptr/make_shared.hpp>

#include <QDateTime>
#include <QSettings>
#include <QUrl>

#include <QTimer>

#include <QDebug>

#include <Swiften/Elements/DiscoInfo.h>
#include <Swiften/Elements/DiscoItems.h>

#include <Swiften/Base/IDGenerator.h>

#include "EchoPayload.h"
#include "RosterContoller.h"
#include "Persistence.h"
#include "MessageController.h"

#include "HttpFileUploadManager.h"
#include "DownloadManager.h"
#include "XmppPingController.h"
#include "ImageProcessing.h"

#include "System.h"

Shmoose::Shmoose(NetworkFactories* networkFactories, QObject *parent) :
	QObject(parent), rosterController_(new RosterController(this)),
	persistence_(new Persistence(this)),
	httpFileUploadManager_(new HttpFileUploadManager(this)),
	downloadManager_(new DownloadManager()),
	xmppPingController_(new XmppPingController()),
	jid_(""), password_("")
{
	netFactories_ = networkFactories;
	connected = false;

	connect(httpFileUploadManager_, SIGNAL(fileUploadedForJidToUrl(QString,QString,QString)),
			this, SLOT(sendMessage(QString,QString,QString)));

#ifdef SFOS
	// just a test to prevent interruption of long term tcp connection
	QTimer *timer = new QTimer(this);
	connect(timer, SIGNAL(timeout()), this, SLOT(doXmppPingIfConnected()));
	timer->start(60000);
#endif
}

Shmoose::~Shmoose()
{
	if (connected)
	{
		client_->removePayloadSerializer(&echoPayloadSerializer_);
		client_->removePayloadParserFactory(&echoPayloadParserFactory_);
		softwareVersionResponder_->stop();
		delete tracer_;
		delete softwareVersionResponder_;
		delete client_;
	}

	delete downloadManager_;
	delete xmppPingController_;
}

void Shmoose::mainConnect(const QString &jid, const QString &pass)
{
	client_ = new Swift::Client(jid.toStdString(), pass.toStdString(), netFactories_);
	client_->setAlwaysTrustCertificates();

	client_->onConnected.connect(boost::bind(&Shmoose::handleConnected, this));
	client_->onDisconnected.connect(boost::bind(&Shmoose::handleDisconnected, this, _1));

	client_->onMessageReceived.connect(
				boost::bind(&Shmoose::handleMessageReceived, this, _1));
	client_->onPresenceReceived.connect(
				boost::bind(&Shmoose::handlePresenceReceived, this, _1));


	tracer_ = new Swift::ClientXMLTracer(client_);

	softwareVersionResponder_ = new Swift::SoftwareVersionResponder(client_->getIQRouter());
	softwareVersionResponder_->setVersion("Shmoose", "0.1");
	softwareVersionResponder_->start();

	client_->addPayloadParserFactory(&echoPayloadParserFactory_);
	client_->addPayloadSerializer(&echoPayloadSerializer_);

	client_->connect();

	// for saving on connection success
	if (checkSaveCredentials() == true)
	{
		jid_ = jid;
		password_ = pass;
	}
}

void Shmoose::setCurrentChatPartner(QString const &jid)
{
	persistence_->setCurrentChatPartner(jid);
}

void Shmoose::sendMessage(QString const &toJid, QString const &message, QString const &type)
{
	Swift::Message::ref msg(new Swift::Message);
	Swift::JID receiverJid(toJid.toStdString());

	Swift::IDGenerator idGenerator;
	std::string msgId = idGenerator.generateID();

	msg->setFrom(JID(client_->getJID()));
	msg->setTo(receiverJid);
	msg->setID(msgId);
	msg->setBody(message.toStdString());
	msg->addPayload(boost::make_shared<DeliveryReceiptRequest>());

	client_->sendMessage(msg);
	persistence_->addMessage(QString::fromStdString(msgId), QString::fromStdString(receiverJid.toBare().toString()), message, type, 0);
	unAckedMessageIds_.push_back(QString::fromStdString(msgId));
}

void Shmoose::sendFile(QString const &toJid, QString const &file)
{
	if (httpFileUploadManager_->requestToUploadFileForJid(file, toJid) == false)
	{
		qDebug() << "Shmoose::sendFile failed";
	}
}

void Shmoose::mainDisconnect()
{
	if (connectionState())
	{
		client_->disconnect();
	}
}

void Shmoose::handlePresenceReceived(Presence::ref presence)
{
	// Automatically approve subscription requests
	if (presence->getType() == Swift::Presence::Subscribe)
	{
		Swift::Presence::ref response = Swift::Presence::create();
		response->setTo(presence->getFrom());
		response->setType(Swift::Presence::Subscribed);
		client_->sendPresence(response);
	}
}

void Shmoose::handleStanzaAcked(Stanza::ref stanza)
{
	if (unAckedMessageIds_.contains(QString::fromStdString(stanza->getID())))
	{
		// FIXME remove from list
		qDebug() << "acked id: " << QString::fromStdString(stanza->getID());
		persistence_->markMessageAsSentById(QString::fromStdString(stanza->getID()));
	}
}

void Shmoose::handleConnected()
{
	connected = true;
	emit connectionStateConnected();

	client_->sendPresence(Presence::create("Send me a message"));

	// register capabilities
	// http://xmpp.org/extensions/xep-0184.html, MessageDeliveryReceiptsFeature
	DiscoInfo discoInfo;
	discoInfo.addIdentity(DiscoInfo::Identity("shmoose", "client", "phone"));
	discoInfo.addFeature(DiscoInfo::MessageDeliveryReceiptsFeature);
	//client_->getDiscoManager()->setCapsNode("https://github.com/geobra/harbour-shmoose");
	client_->getDiscoManager()->setDiscoInfo(discoInfo);

	// Request the roster
	rosterController_->requestRosterFromClient(client_);

	// request the discoInfo from server
	GetDiscoInfoRequest::ref discoInfoRequest = GetDiscoInfoRequest::create(JID(client_->getJID().getDomain()), client_->getIQRouter());
	discoInfoRequest->onResponse.connect(boost::bind(&Shmoose::handleServerDiscoInfoResponse, this, _1, _2));
	discoInfoRequest->send();

	// pass the client pointer to the httpFileUploadManager
	httpFileUploadManager_->setClient(client_);
	xmppPingController_->setClient(client_);

	// xep 198 stream management
	if (client_->getStreamManagementEnabled() == true)
	{
		client_->onStanzaAcked.connect(
					boost::bind(&Shmoose::handleStanzaAcked, this, _1));
	}

	// Save account data
	QSettings settings;
	settings.setValue("authentication/jid", jid_);
	settings.setValue("authentication/password", password_);
}

void Shmoose::handleDisconnected(const boost::optional<ClientError>& error)
{
	connected = false;
	emit connectionStateDisconnected();

	if (error)
	{
		ClientError clientError = *error;
		Swift::ClientError::Type type = clientError.getType();
		qDebug() << "disconnet error: " << type;
	}
	else
	{
		// no error, try a reconnect
		qDebug() << "disconnect without error";
	}

	// FIXME only on a drop of tcp connection
	//client_->connect();
}

void Shmoose::handleMessageReceived(Message::ref message)
{
	//std::cout << "handleMessageReceived" << std::endl;

	std::string fromJid = message->getFrom().toBare().toString();
	boost::optional<std::string> fromBody = message->getBody();

	if (fromBody)
	{
		std::string body = *fromBody;
		QString theBody = QString::fromStdString(body);

		QString type = "txt";

		if (QUrl(theBody).isValid()) // it's an url
		{
			QStringList knownImageTypes = ImageProcessing::getKnownImageTypes();
			QString bodyEnd = theBody.trimmed().right(3); // url ends with an image type
			if (knownImageTypes.contains(bodyEnd))
			{
				type = "image";
				downloadManager_->doDownload(QUrl(theBody));
			}
		}
		persistence_->addMessage(QString::fromStdString(message->getID()), QString::fromStdString(fromJid), theBody, type, 1 );
	}

	// XEP 0184
	if (message->getPayload<DeliveryReceiptRequest>())
	{
		// send message receipt
		Message::ref receiptReply = boost::make_shared<Message>();
		receiptReply->setFrom(message->getTo());
		receiptReply->setTo(message->getFrom());

		boost::shared_ptr<DeliveryReceipt> receipt = boost::make_shared<DeliveryReceipt>();
		receipt->setReceivedID(message->getID());
		receiptReply->addPayload(receipt);
		client_->sendMessage(receiptReply);
	}

	// mark sent msg as received
	DeliveryReceipt::ref rcpt = message->getPayload<DeliveryReceipt>();
	if (rcpt)
	{
		std::string recevideId = rcpt->getReceivedID();
		if (recevideId.length() > 0)
		{
			persistence_->markMessageAsReceivedById(QString::fromStdString(recevideId));
		}
	}
}

void Shmoose::handleServerDiscoInfoResponse(boost::shared_ptr<DiscoInfo> info, ErrorPayload::ref error)
{
	//qDebug() << "Shmoose::handleServerDiscoInfoResponse";
	const std::string httpUpload = "urn:xmpp:http:upload";

	if (!error)
	{
		if (info->hasFeature(httpUpload))
		{
			qDebug() << "has feature urn:xmpp:http:upload";
			//severHasFeatureHttpUpload = true;
			httpFileUploadManager_->setSeverHasFeatureHttpUpload(true);

			foreach (Swift::Form::ref form, info->getExtensions())
			{
				if (form)
				{
					//qDebug() << "form: " << QString::fromStdString((*form).getFormType());
					if ((*form).getFormType() == httpUpload)
					{
						Swift::FormField::ref formField = (*form).getField("max-file-size");
						if (formField)
						{
							unsigned int maxFileSize = std::stoi((*formField).getTextSingleValue());
							qDebug() << QString::fromStdString((*formField).getName()) << " val: " << maxFileSize;
							httpFileUploadManager_->setMaxFileSize(maxFileSize);
						}
					}
				}
			}
		}
	}
}

RosterController* Shmoose::getRosterController()
{
	return rosterController_;
}

Persistence* Shmoose::getPersistence()
{
	return persistence_;
}

bool Shmoose::connectionState() const
{
	return connected;
}

bool Shmoose::checkSaveCredentials()
{
	bool save = false;

	QSettings settings;
	save = settings.value("authentication/saveCredentials", false).toBool();

	return save;
}

void Shmoose::saveCredentials(bool save)
{
	QSettings settings;
	settings.setValue("authentication/saveCredentials", save);
}


QString Shmoose::getJid()
{
	QString returnValue = "";

	QSettings settings;
	if(settings.value("authentication/jid").toString() != "NOT_SET")
	{
		returnValue = settings.value("authentication/jid").toString();
	}

	return returnValue;
}

QString Shmoose::getPassword()
{
	QString returnValue = "";

	QSettings settings;
	if(settings.value("authentication/password").toString() != "NOT_SET")
	{
		returnValue = settings.value("authentication/password").toString();
	}

	return returnValue;
}

QString Shmoose::getAttachmentPath()
{
	return System::getAttachmentPath();
}

void Shmoose::doXmppPingIfConnected()
{
	// FIXME only if inet connection and xmpp server connection available
	xmppPingController_->doPing();
}
