#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <errno.h>
#include <signal.h>

#include "MessagePacket.h"

#define MULTICAST_PORT 5432
#define MULTICAST_GROUP "224.0.0.1"

#define MSGLEN 1024

int sock, sock_send; // Descriptor de socket
struct sockaddr_in multicastAddr; // Estructura de direcciones de multidifusión

/* VARIABLES PARA EL GRUPO MULTICAST */
unsigned char mcastTTL; // TTL de multidifusión
char *mcastIP; // Dirección IP de multidifusión 
unsigned int mcastPort; // Puerto de multidifusión

/* VARIABLES PARA EL SERVIDOR */
unsigned short servPort; // Puerto del servidor
char* servIP; // IP del servidor
struct sockaddr_in servAddr; // Estructura de direcciones del servidor

// Estructura para la configuración del manejador de señales
struct sigaction sigAction; // Signal handler

/* VARIABLES PARA LOS MIEMBROS DEL GRUPO MULTICAST */
char members[100][100]; // Listado de miembros del grupo
int is_connect[100]; // Listado de miembros conectados
int id = 0; // ID del miembro

char groupInfoMsg[MSGLEN]; // Mensaje de información del grupo

/* INFORMACION DE MIEMBROS DE LOS GRUPOS */
char groupMemberStrRow[MSGLEN]; // Fila de la tabla de miembros
char groupMemberStr[MSGLEN * 100]; // Tabla de miembros

/* MANEJADOR DE SEÑALES CUANDO OCURRE SIGIO */
void IOSignalHandler(int signo);

/* FUNCION PARA EMPAQUETAR UN MENSAJE */
int Packetize(short msgID, char *msgBuf, short msgLen, char *pktBuf, int pktBufSize){
  if(msgLen > MSGLEN - 4){
    return -1;
  }

  memcpy(&pktBuf[0], &msgID, 2);
  memcpy(&pktBuf[2], &msgLen, 2);
  memcpy(&pktBuf[4], msgBuf, msgLen);

  return msgLen + 4;
}

/* FUNCION PARA DESEMPAQUETAR */
int Depacketize(char *pktBuf, int pktLen, short *msgID, char *msgBuf, short msgBufSize){
  if(msgBufSize != MSGLEN){
    return -1;
  }

  memcpy(msgID, &pktBuf[0], 2);
  memcpy(&msgBufSize, &pktBuf[2], 2);
  memcpy(msgBuf, &pktBuf[4], msgBufSize);

  return msgBufSize;
}

/* FUNCION PARA PROCESAR LA SALIDA DEL GRUPO */
int leave_member(char *targetUserName){
  int i;
  for(i = 0; i <= 99; i++){
    // Si el nombre de usuario coincide
    if(strcmp(members[i], targetUserName) == 0){
      is_connect[i] = 0; // Desconectar
      return 0;
    }
  }

  return -1;
}

int main(int argc, char * argv[]) {

    // Comprobar los argumentos
    if (argc != 5) {
        fprintf(stderr, "Usage: %s<Listen IP> <Listen Port> <Group IP> <Group Port>\n", argv[0]);
        exit(1);
    }

    // Inicialización de la tabla de miembros
    int i;
    for (i = 0; i <= 99; i++){
        is_connect[i] = 0;
        strcpy(members[i], "");
    }

    servIP = argv[1];
    servPort = atoi(argv[2]);

    // Creación de un socket
    if((sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0){
        perror("socket() failed");
        exit(1);
    }

    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = inet_addr(servIP);
    servAddr.sin_port = htons(servPort);

    
    if(bind(sock, (struct sockaddr *) &servAddr, sizeof(servAddr)) < 0){
        perror("bind() failed");
        exit(1);
    }

    mcastIP = argv[3];
    mcastPort = atoi(argv[4]);
    mcastTTL = 1; // TTL de multidifusión

    // Cree un socket para usar para enviar mensajes.
    sock_send = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (setsockopt(sock_send, IPPROTO_IP, IP_MULTICAST_TTL, (void*)&mcastTTL, sizeof(mcastTTL)) < 0) {
        fprintf(stderr, "setsockopt() failed\n");
        exit(1);
    }

    // Configuración de la estructura de direcciones de multidifusión
    memset(&multicastAddr, 0, sizeof(multicastAddr));
    multicastAddr.sin_family = AF_INET;
    multicastAddr.sin_addr.s_addr = inet_addr(mcastIP); 
    multicastAddr.sin_port = htons(mcastPort);

    // Establecer controlador de señal
    sigAction.sa_handler = IOSignalHandler;

    // Configure las señales para bloquear en el controlador (bloquee todas las señales)
    if (sigfillset(&sigAction.sa_mask) < 0) {
        fprintf(stderr, "sigfillset() failed\n");
        exit(1);
    }

    // Sin banderas
    sigAction.sa_flags = 0;

    // Registre un manejador de señales usando la estructura de configuración del manejador de señales
    if (sigaction(SIGIO, &sigAction, 0) < 0) {
        fprintf(stderr, "sigaction() failed for SIGIO\n");
        exit(1);
    }

    // Establecer el propietario del socket para recibir señales
    if (fcntl(sock, F_SETOWN, getpid()) < 0) {
        fprintf(stderr, "Unable to set process owner to us\n");
        exit(1);
    }

    // Configura este proceso para recibir señales relacionadas con sockets.
    if (fcntl(sock, F_SETFL, O_NONBLOCK | FASYNC) < 0) {
        fprintf(stderr, "Unable to put the sock into nonblocking/async mode\n");
        exit(1);
    }

    printf("[server] Servidor iniciado, en dirección %s y puerto %d\n", inet_ntoa(servAddr.sin_addr), ntohs(servAddr.sin_port));
    printf("[server] Grupo de multidifusión en %s:%d\n", mcastIP, mcastPort);
    
    // Bucle infinito
    for(;;){
        sleep(1);
    }

}

// Manejador de señales cuando ocurre SIGIO
void IOSignalHandler(int signo){
    struct sockaddr_in clientAddr; // Dirección del cliente
    unsigned int clientAddrLen; // Longitud de la dirección del cliente
    char pktBuffer[MSGLEN]; // Buffer de paquetes
    int recvPktLen; // Longitud del paquete recibido
    int sendMsjLen; // Longitud del mensaje enviado

    int msgID, pktLen; // ID del mensaje, longitud del paquete
    char msgBuffer[MSGLEN]; // Buffer de mensajes
    int msgBufSize; // Tamaño del buffer de mensajes

    // Repita la recepción y transmisión hasta que no se reciban más datos.
    do {
        clientAddrLen = sizeof(clientAddr);

        recvPktLen = recvfrom(sock, pktBuffer, MSGLEN, 0, (struct sockaddr *) &clientAddr, &clientAddrLen);

        if(recvPktLen < 0){
            //Si el error es EWOULDBLOCK, indica que no se reciben más datos.
            if (errno!= EWOULDBLOCK){
                fprintf(stderr, "recvfrom() failed\n");
                exit(1);
            }
        } else {
            // Recibir paquetes
            msgBufSize = Depacketize(pktBuffer, recvPktLen, (short *)&msgID, msgBuffer, MSGLEN);
            msgBuffer[msgBufSize] = '\0';

            // Solicitud de participación en grupo
            if(msgID == MSG_ID_JOIN_REQUEST){
                printf("[server] Solicitud de participación en grupo recibida, de %s, ID asignado %d\n", msgBuffer, id);
                // Responder con un mensaje de información del grupo
                strcpy(members[id], msgBuffer);
                // Conectar
                is_connect[id++] = 1;

                pktLen = Packetize(MSG_ID_JOIN_RESPONSE, msgBuffer, strlen(msgBuffer), pktBuffer, sizeof(pktBuffer));

                sendMsjLen = sendto(sock_send, pktBuffer, pktLen, 0, (struct sockaddr *) &multicastAddr, sizeof(multicastAddr));
            }

            // Solicitud de información de grupo
            else if(msgID == MSG_ID_GROUP_INFO_REQUEST){
                printf("[server] Solicitud de información de grupo recibida de %s\n", msgBuffer);

                // Construir una tabla de miembros del grupo
                snprintf(groupInfoMsg, MSGLEN, "%s:%d\n", MULTICAST_GROUP, MULTICAST_PORT);
                snprintf(groupInfoMsg, MSGLEN, "%s, solicitado por %s", groupInfoMsg, msgBuffer);

                pktLen = Packetize(MSG_ID_GROUP_INFO_RESPONSE, groupInfoMsg, strlen(groupInfoMsg), pktBuffer, sizeof(pktBuffer));

                sendMsjLen = sendto(sock_send, pktBuffer, pktLen, 0, (struct sockaddr *) &multicastAddr, sizeof(multicastAddr));
            }


            else if(msgID == MSG_ID_USER_LIST_REQUEST){
                printf("[server] Solicitud de lista de usuarios recibida de %s\n", msgBuffer);

                snprintf(groupMemberStr, MSGLEN*100, "\n\n[*] Solicitud del miembro %s%s\n","", msgBuffer);

                snprintf(groupMemberStr, MSGLEN*100,"%s%s\n",groupMemberStr,"---Miembros del grupo---\n");

                for(int i = 0;i < id; i++){
                    if(is_connect[i]){
                        snprintf(groupMemberStr, MSGLEN*100, "\t%s%s\n", groupMemberStr, members[i]);
                    }else{
                        snprintf(groupMemberStr, MSGLEN*100, "\t%s%s (Desconectado)\n", groupMemberStr, members[i]);
                    }
                }

                pktLen = Packetize(MSG_ID_USER_LIST_RESPONSE, groupMemberStr, strlen(groupMemberStr), pktBuffer, sizeof(pktBuffer));

                sendMsjLen = sendto(sock_send, pktBuffer, pktLen, 0, (struct sockaddr *) &multicastAddr, sizeof(multicastAddr));
            }

            // Solicitud de salida del grupo
            else if(msgID == MSG_ID_LEAVE_REQUEST){
                printf("[server] Solicitud de salida del grupo recibida de %s\n", msgBuffer);

                if(leave_member(msgBuffer) == 0){
                    pktLen = Packetize(MSG_ID_LEAVE_RESPONSE, msgBuffer, strlen(msgBuffer), pktBuffer, sizeof(pktBuffer));
                    sendMsjLen = sendto(sock_send, pktBuffer, pktLen, 0,(struct sockaddr*)&multicastAddr, sizeof(multicastAddr));
                } else {
                    printf("[server] Solicitud de salida del grupo rechazada. ERROR!\n");
                    return;
                }
            }

            // Mensaje de chat privado
            else if(msgID == MSG_ID_PRIVATE_CHAT_TEXT){
                printf("[*] %s", msgBuffer);
                pktLen = Packetize(MSG_ID_PRIVATE_CHAT_TEXT, msgBuffer, msgBufSize, pktBuffer, sizeof(pktBuffer));

                // Enviar mensaje recibido tal cual al grupo de multidifusión
                sendMsjLen = sendto(sock_send, pktBuffer, pktLen, 0,(struct sockaddr*)&multicastAddr, sizeof(multicastAddr));
            }

            // Mensaje de chat
            else if(msgID == MSG_ID_CHAT_TEXT){
                printf("[*] %s", msgBuffer);
                pktLen = Packetize(msgID, msgBuffer, msgBufSize, pktBuffer, sizeof(pktBuffer));

                // Enviar mensaje recibido tal cual al grupo de multidifusión
                sendMsjLen = sendto(sock_send, pktBuffer, pktLen, 0,(struct sockaddr*)&multicastAddr, sizeof(multicastAddr));
            }
        }
    } while (recvPktLen >= 0);
}
