#include "GamepadMidiController.h"

using namespace std;

int main()
{
	// Create the midi object
	libremidi::midi_out midi;

	std::cout << "Available MIDI input ports:\n";
	libremidi::observer obs;
	libremidi::output_port out_port;
	for (const libremidi::output_port& port : obs.get_output_ports()) 
	{
		if (port.port_name.find("loop") != std::string::npos)
		{
			out_port = port;
		}
	}

	std::cout << "Port: " << out_port.port_name << std::endl;

	midi.open_port(out_port);

	return 0;
}
