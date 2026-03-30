Implementation of a wind farm supervisory controller in C++, based on the IEC61850-7 communication protocol, with IEC61400-25 data information model. 

The implemented IEC61850 is planned to support both MMS for low-frequency monitoring and control, and GOOSE for fast response signalling.

There are two optional zeromq socket available for receiving some operator references/control and an interface to read and write all the communication occuring over MMS.
