#pragma once

#include <string>
#include <vector>

namespace IEC_STRINGS {

inline constexpr const char* WTUR_DmdWSpt = "WTUR1.DmdWSpt.val";
inline constexpr const char* XWYAW_YawSpt = "WYAW1.YwAngSpt.val";
inline constexpr const char* WTUR_OP_CMD = "WTUR1.TurOp.st";
inline constexpr const char* WTUR_OP_CMD_VAL = "WTUR1.TurOp.st.stVal";
inline constexpr const char* WTUR_TURCTL = "WTUR1.TurCtl.st";
inline constexpr const char* WTUR_TURCTL_VAL = "WTUR1.TurCtl.st.stVal";

inline constexpr const char* WTUR_TurSt = "WTUR1.TurSt";
inline constexpr const char* POWER_MEAS = "WTUR1.W.mag.f";
inline constexpr const char* YAW_MEAS = "WYAW1.YwAng.mag.f";
inline constexpr const char* WS_MEAS = "WMET1.HorWdSpd.mag.f";
inline constexpr const char* WD_MEAS = "WMET1.HorWdDir.mag.f";
inline constexpr const char* RPM_MEAS = "WROT1.RotSpd.mag.f";
inline constexpr const char* TOT_W = "WTUR1.TotWh.f";
inline constexpr const char* GEN_TORQ = "WCNV1.Torq.mag.f";
inline constexpr const char* PITCH_SP = "WROT1.BlPthAngTgt.f";
inline constexpr const char* PITCH_VAL = "WROT1.BlPthAngVal.f";
inline constexpr const char* SECR_S = "SECR1.S.stVal";

inline const std::vector<std::string> REQ_CMDS = {
    WTUR_DmdWSpt, XWYAW_YawSpt, WTUR_OP_CMD, WTUR_TURCTL};
inline const std::vector<std::string> REQ_REFS = {
    POWER_MEAS, YAW_MEAS, WS_MEAS, WD_MEAS, RPM_MEAS, TOT_W, PITCH_VAL, SECR_S};

inline constexpr const char* GOOSE_SUB_TEST = "LLN0$gocb01";
inline constexpr const char* GOOSE_SUB_TurSt = "WTUR1$GO$TurSt";
inline constexpr const char* GOOSE_SUB_Alm = "WTUR1$GO$Alm";

} // namespace IEC_STRINGS

struct TurbineEndpoint {
    std::string host;
    int port{102};
    std::string iedName;
    std::string logicalDevice{"WTGLD1"};
    std::vector<std::string> gooseRefs{IEC_STRINGS::GOOSE_SUB_TurSt};
};
