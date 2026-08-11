#include "weather.h"

/* Codigos WMO tal como los devuelve Open-Meteo; el celular manda solo el numero. */
const char *weather_wmo_desc(int code)
{
    switch (code) {
    case 0:  return "DESPEJADO";
    case 1:  return "CASI CLARO";
    case 2:  return "PARC NUBLADO";
    case 3:  return "NUBLADO";
    case 45:
    case 48: return "NIEBLA";
    case 51:
    case 53:
    case 55: return "LLOVIZNA";
    case 56:
    case 57: return "LLOV HELADA";
    case 61:
    case 63: return "LLUVIA";
    case 65: return "LLUVIA FUERTE";
    case 66:
    case 67: return "LLUVIA HELADA";
    case 71:
    case 73:
    case 75:
    case 77: return "NIEVE";
    case 80:
    case 81: return "CHUBASCOS";
    case 82: return "AGUACERO";
    case 85:
    case 86: return "NEVADAS";
    case 95: return "TORMENTA";
    case 96:
    case 99: return "TORMENTA ELEC";
    default: return "SIN DATO";
    }
}

/*
 * Rangos de la tabla WMO: 51-57 llovizna, 61-67 lluvia, 71-77 nieve,
 * 80-86 chubascos y nevadas, 95-99 tormenta. La niebla (45-48) no cuenta:
 * moja, pero no es algo de lo que valga la pena taparse.
 */
bool weather_hay_precipitacion(int code)
{
    return (code >= 51 && code <= 67)
        || (code >= 71 && code <= 77)
        || (code >= 80 && code <= 86)
        || (code >= 95 && code <= 99);
}

bool weather_es_despejado(int code)
{
    return code == 0 || code == 1;
}
