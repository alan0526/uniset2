#include <sstream>
#include "Exceptions.h"
#include "Extensions.h"
#include "UNetSender.h"
// -----------------------------------------------------------------------------
using namespace std;
using namespace UniSetTypes;
using namespace UniSetExtensions;
// -----------------------------------------------------------------------------
UNetSender::UNetSender( const std::string s_host, const ost::tpport_t port, SMInterface* smi,
						const std::string s_f, const std::string s_val, SharedMemory* ic ):
s_field(s_f),
s_fvalue(s_val),
shm(smi),
s_host(s_host),
sendpause(150),
activated(false),
dlist(100),
maxItem(0),
packetnum(1),
s_thr(0)
{

	{
		ostringstream s;
		s << "(" << s_host << ":" << port << ")";
		myname = s.str();
	}

	// определяем фильтр
//	s_field = conf->getArgParam("--udp-filter-field");
//	s_fvalue = conf->getArgParam("--udp-filter-value");
	dlog[Debug::INFO] << myname << "(init): read fileter-field='" << s_field
						<< "' filter-value='" << s_fvalue << "'" << endl;

	if( dlog.debugging(Debug::INFO) )
		dlog[Debug::INFO] << "(UNetSender): UDP set to " << s_host << ":" << port << endl;


	try
	{
		addr = s_host.c_str();
		udp = new ost::UDPBroadcast(addr,port);
	}
	catch( ost::SockException& e )
	{
		ostringstream s;
		s << e.getString() << ": " << e.getSystemErrorString() << endl;
		throw SystemError(s.str());
	}

	s_thr = new ThreadCreator<UNetSender>(this, &UNetSender::send);

	// -------------------------------
	if( shm->isLocalwork() )
	{
		readConfiguration();
		dlist.resize(maxItem);
		dlog[Debug::INFO] << myname << "(init): dlist size = " << dlist.size() << endl;
	}
	else
		ic->addReadItem( sigc::mem_fun(this,&UNetSender::readItem) );


	// выставляем поля, которые не меняются
	mypack.msg.header.nodeID = conf->getLocalNode();
	mypack.msg.header.procID = shm->ID();
}
// -----------------------------------------------------------------------------
UNetSender::~UNetSender()
{
	delete s_thr;
	delete udp;
	delete shm;
}
// -----------------------------------------------------------------------------
void UNetSender::update( UniSetTypes::ObjectId id, long value )
{
	DMap::iterator it=dlist.begin();
	for( ; it!=dlist.end(); ++it )
	{
		if( it->si.id == id )
		{
			uniset_spin_lock lock(it->val_lock);
			it->val = value;
		}
		break;
	}
}
// -----------------------------------------------------------------------------
void UNetSender::send()
{
	dlist.resize(maxItem);
	dlog[Debug::INFO] << myname << "(init): dlist size = " << dlist.size() << endl;
/*
	ost::IPV4Broadcast h = s_host.c_str();
	try
	{
		udp->setPeer(h,port);
	}
	catch( ost::SockException& e )
	{
		ostringstream s;
		s << e.getString() << ": " << e.getSystemErrorString();
		dlog[Debug::CRIT] << myname << "(poll): " << s.str() << endl;
		throw SystemError(s.str());
	}
*/
	while( activated )
	{
		try
		{
			real_send();
		}
		catch( ost::SockException& e )
		{
			cerr  << e.getString() << ": " << e.getSystemErrorString() << endl;
		}
		catch( UniSetTypes::Exception& ex)
		{
			cerr << myname << "(send): " << ex << std::endl;
		}
		catch(...)
		{
			cerr << myname << "(send): catch ..." << std::endl;
		}

		msleep(sendpause);
	}

	cerr << "************* execute FINISH **********" << endl;
}
// -----------------------------------------------------------------------------
void UNetSender::real_send()
{
	mypack.msg.header.num = packetnum++;

	if( packetnum > UniSetUDP::MaxPacketNum )
		packetnum = 1;

//	cout << "************* send header: " << mypack.msg.header << endl;
	int sz = mypack.byte_size() + sizeof(UniSetUDP::UDPHeader);
	if( !udp->isPending(ost::Socket::pendingOutput) )
		return;

	size_t ret = udp->send( (char*)&(mypack.msg),sz);
	if( ret < sz )
		dlog[Debug::CRIT] << myname << "(send): FAILED ret=" << ret << " < sizeof=" << sz << endl;
}
// -----------------------------------------------------------------------------
void UNetSender::start()
{
	if( !activated )
	{
		activated = true;
		s_thr->start();
	}
}
// -----------------------------------------------------------------------------
void UNetSender::readConfiguration()
{
	xmlNode* root = conf->getXMLSensorsSection();
	if(!root)
	{
		ostringstream err;
		err << myname << "(readConfiguration): not found <sensors>";
		throw SystemError(err.str());
	}

	UniXML_iterator it(root);
	if( !it.goChildren() )
	{
		std::cerr << myname << "(readConfiguration): empty <sensors>?!!" << endl;
		return;
	}

	for( ;it.getCurrent(); it.goNext() )
	{
		if( check_item(it) )
			initItem(it);
	}
}
// ------------------------------------------------------------------------------------------
bool UNetSender::check_item( UniXML_iterator& it )
{
	if( s_field.empty() )
		return true;

	// просто проверка на не пустой field
	if( s_fvalue.empty() && it.getProp(s_field).empty() )
		return false;

	// просто проверка что field = value
	if( !s_fvalue.empty() && it.getProp(s_field)!=s_fvalue )
		return false;

	return true;
}
// ------------------------------------------------------------------------------------------
bool UNetSender::readItem( UniXML& xml, UniXML_iterator& it, xmlNode* sec )
{
	if( check_item(it) )
		initItem(it);
	return true;
}
// ------------------------------------------------------------------------------------------
bool UNetSender::initItem( UniXML_iterator& it )
{
	string sname( it.getProp("name") );

	string tid = it.getProp("id");

	ObjectId sid;
	if( !tid.empty() )
	{
		sid = UniSetTypes::uni_atoi(tid);
		if( sid <= 0 )
			sid = DefaultObjectId;
	}
	else
		sid = conf->getSensorID(sname);

	if( sid == DefaultObjectId )
	{
		if( dlog )
			dlog[Debug::CRIT] << myname << "(readItem): ID not found for "
							<< sname << endl;
		return false;
	}

	UItem p;
	p.si.id = sid;
	p.si.node = conf->getLocalNode();
	mypack.addData(sid,0);
	p.pack_ind = mypack.size()-1;

	if( maxItem >= mypack.size() )
		dlist.resize(maxItem+10);

	dlist[maxItem] = p;
	maxItem++;

	if( dlog.debugging(Debug::INFO) )
		dlog[Debug::INFO] << myname << "(initItem): add " << p << endl;

	return true;
}

// ------------------------------------------------------------------------------------------
void UNetSender::initIterators()
{
	DMap::iterator it=dlist.begin();
	for( ; it!=dlist.end(); it++ )
	{
		shm->initDIterator(it->dit);
		shm->initAIterator(it->ait);
	}
}
// -----------------------------------------------------------------------------
std::ostream& operator<<( std::ostream& os, UNetSender::UItem& p )
{
	return os 	<< " sid=" << p.si.id;
}
// -----------------------------------------------------------------------------