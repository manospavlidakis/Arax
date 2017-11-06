#include <vine_pipe.h>
#include <arch/alloc.h>
#include <core/vine_object.h>
#include <stdio.h>
#include "Poco/Net/HTTPServer.h"
#include "Poco/Util/ServerApplication.h"
#include "Poco/Net/HTTPRequestHandler.h"
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/URI.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <random>
#include <set>
#include <unistd.h>
#include <map>
#include "Misc.h"
#include "WebUI.h"
#include "Collector.h"

using namespace Poco;
using namespace Poco::Util;
using namespace Poco::Net;


char hostname[1024];

vine_pipe_s *vpipe;

std::map<std::string,bool> args;

std::vector<std::string> pallete;

const char * ns_to_secs[] = {"ns","us","ms","s"};

int bar_count = 0;

class WebUIFactory : public HTTPRequestHandlerFactory
{
public:
	virtual HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& rq)
	{
		return new WebUI(args);
	}
};

class Server : public ServerApplication
{
	void initialize(Application & self)
	{
		std::cerr << "VDF at port " << port << std::endl;
		webui = new HTTPServer(new WebUIFactory(),port,new HTTPServerParams());
		collector = new Collector(port+1);
	}

	int main(const std::vector < std::string > & args)
	{
		webui->start();
		collector->start();
		waitForTerminationRequest();
		collector->stop();
		webui->stop();
	}

	HTTPServer *webui;
	TCPServer *collector;
	int port;

	public:
		Server(int port)
		:port(port)
		{}
};



int main(int argc,char * argv[])
{
	int port = 8888;

	for(int arg = 0 ; arg < argc ; arg++)
	{
		if(atoi(argv[arg]))
			port = atoi(argv[arg]);
		else
			args[argv[arg]] = true;
	}

	gethostname(hostname,1024);

	for(int r = 0 ; r < 16 ; r++)
		for(int g = 0 ; g < 16 ; g++)
			for(int b = 0 ; b < 16 ; b++)
			{
				std::string c = "";
				if(r < 10)
					c += '0'+r;
				else
					c += 'A'+(r-10);
				if(g < 10)
					c += '0'+g;
				else
					c += 'A'+(g-10);
				if(b < 10)
					c += '0'+b;
				else
					c += 'A'+(b-10);
				pallete.push_back(c);
			}
	std::shuffle(pallete.begin(),pallete.end(),std::default_random_engine(0));


	vpipe = vine_talk_init();
	if(!vpipe)
	{
		fprintf(stderr,"Could not get vine_pipe instance!\n");
		return -1;
	}

	Server app(port);
	app.run(argc,argv);

	vine_talk_exit();
	return 0;
}
