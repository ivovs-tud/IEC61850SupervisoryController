Implementation of a wind farm supervisory controller in C++, based on the IEC61850-7 communication protocol, with IEC61400-25 data information model. Intended to interface with https://github.com/ivovs-tud/IEC61850ServerPLC due to custom entries in the IEC61400-25 data information model.

There are two optional zeromq socket available for receiving some operator references/control and an interface to read and write all the communication occuring over MMS. For a more complete description see https://github.com/ivovs-tud/HackAWindFarm.

This repository has been developed as part of the EU Horizon program [SUDOCO](https://sudoco.eu/).
