#include <string>
#include <thread>
#include <signal.h>

#include <argagg/argagg.hpp>

#include <prometheus/exposer.h>
#include <prometheus/registry.h>

#include "connection_table.h"

using namespace std;
using namespace prometheus;
using namespace conntrackex;


// Allow Ctrl+C to break the main loop:
static volatile int keep_running = 1;
void sigint_handler(int)
{
    cout << "Stopping conntrack_exporter" << endl;
    keep_running = 0;
}

int main(int argc, char** argv)
{
    signal(SIGINT, sigint_handler);

    // Parse arguments:
    argagg::parser argument_parser {{

        { "bind_address", {"-b", "--bind-address"}, "The IP address on which to bind the metrics HTTP endpoint (default: 0.0.0.0)", 1 },
        { "listen_port", {"-l", "--listen-port"}, "The port on which to expose the metrics HTTP endpoint (default: 9318)", 1 },
        { "log_events", {"-e", "--log-events"}, "Enables logging of connection events", 0 },
        { "log_events_format", {"-f", "--log-events-format"}, "Connection events log format (netfilter [default] or json)", 1 },
        { "debug", {"-d", "--debug"}, "Enables logging of debug messages", 0 },
        { "help", {"-h", "--help"}, "Print help and exit", 0 },

    }};
    ostringstream usage;
    usage << "Usage: " << argv[0] << " [options]" << endl;
    argagg::fmt_ostream help(cerr);
    argagg::parser_results args;
    try
    {
        args = argument_parser.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        help << "ERROR: " << e.what() << endl
             << endl
             << usage.str() << endl
             << argument_parser << endl;
        return EXIT_FAILURE;
    }
    if (args["help"])
    {
        help << usage.str() << endl
            << argument_parser << endl;
        return EXIT_SUCCESS;
    }

    // Read arguments and set defaults when needed:
    const string bind_address = args["bind_address"] ?
        args["bind_address"].as<std::string>() :
        "0.0.0.0";
    const string guessed_local_endpoint = (bind_address == "0.0.0.0" || bind_address == "127.0.0.1") ?
        "localhost" :
        bind_address;
    const string listen_port = args["listen_port"] ?
        args["listen_port"].as<std::string>() :
        "9318"; // see https://github.com/prometheus/prometheus/wiki/Default-port-allocations

    try
    {
        Exposer exposer{bind_address + ":" + listen_port};
        ConnectionTable table;
        if (args["log_events"])
            table.enableLogging();
        if (args["log_events_format"])
            table.setLoggingFormat(args["log_events_format"]);
        if (args["debug"])
            table.enableDebugging();

        cout << "conntrack_exporter v0.3" << endl;
        cout << "Serving metrics at http://" + guessed_local_endpoint + ":" << listen_port << "/metrics ..." << endl;

        Connection::loadLocalIPAddresses(args["debug"]);

        table.attach();
        while (keep_running) {

            // Build up a registry and metric families:
            auto registry = make_shared<Registry>();
            auto& opening_connections_family = BuildGauge()
                .Name("conntrack_opening_connections")
                .Help("How many connections to the remote host are currently opening?")
                .Register(*registry);
            auto& open_connections_family = BuildGauge()
                .Name("conntrack_open_connections")
                .Help("How many open connections are there to the remote host?")
                .Register(*registry);
            auto& closing_connections_family = BuildGauge()
                .Name("conntrack_closing_connections")
                .Help("How many connections to the remote host are currently closing?")
                .Register(*registry);
            auto& closed_connections_family = BuildGauge()
                .Name("conntrack_closed_connections")
                .Help("How many connections to the remote host have recently closed?")
                .Register(*registry);

            // Add guages for the individual connections:
            table.update();
            for (auto& connection : table.getConnections())
            {
                if (!connection.hasState())
                    continue;

                Gauge* pGuage;
                switch (connection.getState())
                {
                    case ConnectionState::OPENING:
                        pGuage = &opening_connections_family.Add({{"host", connection.getRemoteHost()}});
                        break;
                    case ConnectionState::OPEN:
                        pGuage = &open_connections_family.Add({{"host", connection.getRemoteHost()}});
                        break;
                    case ConnectionState::CLOSING:
                        pGuage = &closing_connections_family.Add({{"host", connection.getRemoteHost()}});
                        break;
                    case ConnectionState::CLOSED:
                        pGuage = &closed_connections_family.Add({{"host", connection.getRemoteHost()}});
                        break;
                    default:
                        continue;
                }
                pGuage->Increment();
            }
            exposer.RegisterCollectable(registry);

            this_thread::sleep_for(chrono::seconds(1));
        }
    }
    catch (const exception& e)
    {
        cout << "ERROR: " << e.what() << endl;
        exit(EXIT_FAILURE);
    }

    return EXIT_SUCCESS;
}
