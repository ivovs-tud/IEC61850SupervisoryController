#pragma once

#include <string>

// ---------------------------------------------------------------------------
// DataHistorian – logging / historian interface stub.
// TODO: implement persistent storage (e.g. InfluxDB, CSV, SQLite).
// ---------------------------------------------------------------------------
class DataHistorian
{
public:
    // Log a named scalar measurement.
    void log(const std::string& key, double value);

    // Flush buffered records to the backing store.
    void flush();
};
