#include "network_interface.hpp"
#ifndef CODE_INLINE
#include "network_interface.ipp"
#endif

namespace KBEngine { 
namespace Mercury
{
const int NetworkInterface::RECV_BUFFER_SIZE = 16 * 1024 * 1024; // 16MB
const char * NetworkInterface::USE_KBEMACHINED = "kbemachined";

//-------------------------------------------------------------------------------------
NetworkInterface::NetworkInterface( Mercury::EventDispatcher * pMainDispatcher,
		NetworkInterfaceType networkInterfaceType,
		uint16 listeningPort, const char * listeningInterface ) :
	socket_(),
	address_( Address::NONE ),
	channelMap_(),
	isExternal_( networkInterfaceType == NETWORK_INTERFACE_EXTERNAL ),
	isVerbose_( true ),
	pDispatcher_( new EventDispatcher ),
	pMainDispatcher_( NULL ),
	pExtensionData_( NULL )
{
	this->recreateListeningSocket( listeningPort, listeningInterface );

	if (pMainDispatcher != NULL)
	{
		this->attach( *pMainDispatcher );
	}
}

//-------------------------------------------------------------------------------------
NetworkInterface::~NetworkInterface()
{
	// Delete any channels this owns.
	ChannelMap::iterator iter = channelMap_.begin();
	while (iter != channelMap_.end())
	{
		ChannelMap::iterator oldIter = iter++;
		Channel * pChannel = oldIter->second;

		if (pChannel->isOwnedByInterface())
		{
			pChannel->destroy();
		}
		else
		{
			WARNING_MSG( "NetworkInterface::~NetworkInterface: "
					"Channel to %s is still registered\n",
				pChannel->c_str() );
		}
	}

	this->detach();

	this->closeSocket();

	delete pDispatcher_;
	pDispatcher_ = NULL;
}

//-------------------------------------------------------------------------------------
void NetworkInterface::attach( EventDispatcher & mainDispatcher )
{
	KBE_ASSERT( pMainDispatcher_ == NULL );
	pMainDispatcher_ = &mainDispatcher;
	mainDispatcher.attach( this->dispatcher() );
}

//-------------------------------------------------------------------------------------
void NetworkInterface::detach()
{
	if (pMainDispatcher_ != NULL )
	{
		pMainDispatcher_->detach( this->dispatcher() );
		pMainDispatcher_ = NULL;
	}
}

//-------------------------------------------------------------------------------------
void NetworkInterface::closeSocket()
{
	if (socket_.good())
	{
		this->dispatcher().deregisterFileDescriptor( socket_ );
		socket_.close();
		socket_.detach();
	}
}

//-------------------------------------------------------------------------------------
bool NetworkInterface::recreateListeningSocket( uint16 listeningPort,
	const char * listeningInterface )
{
	this->closeSocket();

	// clear this unless it gets set otherwise
	address_.ip = 0;
	address_.port = 0;
	address_.salt = 0;

	// make the socket
	socket_.socket( SOCK_DGRAM );

	if (!socket_.good())
	{
		ERROR_MSG( "NetworkInterface::recreateListeningSocket: "
				"couldn't create a socket\n" );
		return false;
	}

	this->dispatcher().registerFileDescriptor(socket_, pPacketReceiver_);

	// ask endpoint to parse the interface specification into a name
	char ifname[IFNAMSIZ];
	uint32 ifaddr = INADDR_ANY;
	bool listeningInterfaceEmpty =
		(listeningInterface == NULL || listeningInterface[0] == 0);

	// Query bwmachined over the local interface (dev: lo) for what it
	// believes the internal interface is.
	if (listeningInterface &&
		(strcmp( listeningInterface, USE_KBEMACHINED ) == 0))
	{
		INFO_MSG( "NetworkInterface::recreateListeningSocket: "
				"Querying KBEMachined for interface\n" );

	}
	else if (socket_.findIndicatedInterface( listeningInterface, ifname ) == 0)
	{
		INFO_MSG( "NetworkInterface::recreateListeningSocket: "
				"Creating on interface '%s' (= %s)\n",
			listeningInterface, ifname );
		if (socket_.getInterfaceAddress( ifname, ifaddr ) != 0)
		{
			WARNING_MSG( "NetworkInterface::recreateListeningSocket: "
				"Couldn't get addr of interface %s so using all interfaces\n",
				ifname );
		}
	}
	else if (!listeningInterfaceEmpty)
	{
		WARNING_MSG( "NetworkInterface::recreateListeningSocket: "
				"Couldn't parse interface spec '%s' so using all interfaces\n",
			listeningInterface );
	}

	// now we know where to bind, so do so
	if (socket_.bind( listeningPort, ifaddr ) != 0)
	{
		ERROR_MSG( "NetworkInterface::recreateListeningSocket: "
				"Couldn't bind the socket to %s (%s)\n",
			Address( ifaddr, listeningPort ).c_str(), strerror( errno ) );
		socket_.close();
		socket_.detach();
		return false;
	}

	// but for advertising it ask the socket for where it thinks it's bound
	socket_.getlocaladdress( (uint16*)&address_.port,
		(uint32*)&address_.ip );

	if (address_.ip == 0)
	{
		// we're on INADDR_ANY, report the address of the
		//  interface used by the default route then
		if (socket_.findDefaultInterface( ifname ) != 0 ||
			socket_.getInterfaceAddress( ifname,
				(uint32&)address_.ip ) != 0)
		{
			ERROR_MSG( "NetworkInterface::recreateListeningSocket: "
				"Couldn't determine ip addr of default interface\n" );

			socket_.close();
			socket_.detach();
			return false;
		}

		INFO_MSG( "NetworkInterface::recreateListeningSocket: "
				"bound to all interfaces with default route "
				"interface on %s ( %s )\n",
			ifname, address_.c_str() );
	}

	INFO_MSG( "NetworkInterface::recreateListeningSocket: address %s\n",
		address_.c_str() );

	socket_.setnonblocking( true );

#if defined( unix ) && !defined( PLAYSTATION3 )
	int recverrs = true;
	setsockopt( socket_, SOL_IP, IP_RECVERR, &recverrs, sizeof(int) );
#endif

#ifdef KBE_SERVER
	if (!socket_.setBufferSize( SO_RCVBUF, RECV_BUFFER_SIZE ))
	{
		WARNING_MSG( "NetworkInterface::recreateListeningSocket: "
			"Operating with a receive buffer of only %d bytes (instead of %d)\n",
			socket_.getBufferSize( SO_RCVBUF ), RECV_BUFFER_SIZE );
	}
#endif

	return true;
}

void NetworkInterface::delayedSend( Channel & channel )
{
	//pDelayedChannels_->add( channel );
}

void NetworkInterface::handleTimeout(TimerHandle handle, void * arg)
{
}

Channel * NetworkInterface::findChannel(const Address & addr)
{
	if (addr.ip == 0)
	{
		return NULL;
	}

	ChannelMap::iterator iter = channelMap_.find( addr );
	Channel * pChannel = iter != channelMap_.end() ? iter->second : NULL;

	return pChannel;
}

void NetworkInterface::onChannelGone( Channel * pChannel )
{
}

void NetworkInterface::onChannelTimeOut( Channel * pChannel )
{
	ERROR_MSG( "NetworkInterface::onChannelTimeOut: Channel %s timed out.\n",
			pChannel->c_str() );
}

void NetworkInterface::send( const Address & address, Bundle & bundle, Channel * pChannel )
{
}

void NetworkInterface::sendPacket( const Address & address,
						Packet * pPacket,
						Channel * pChannel, bool isResend )
{
}

}
}