#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>

#define SERVER_PORT 8888

int main(int argc, char **argv)                                               
{
	int iSocketClient;
	struct sockaddr_in tSocketServerAddr;
	 
	int iRet;
	unsigned char ucSendBuf[1000];
	int iSendLen;

	if (argc != 2)
	{
		printf("Usage:\n");
		printf("%s <server_ip>\n", argv[0]);     //�ڿͻ�����Ҫд��Ҫ���ӵ�server_ip
		return -1;
	}

	iSocketClient = socket(AF_INET, SOCK_STREAM, 0);        

	tSocketServerAddr.sin_family      = AF_INET;
	tSocketServerAddr.sin_port        = htons(SERVER_PORT);  /* host to net, short */
 	//tSocketServerAddr.sin_addr.s_addr = INADDR_ANY;
 	if (0 == inet_aton(argv[1], &tSocketServerAddr.sin_addr))//ת�����洢���ṹ��  int inet_aton(const char *cp = ʮ���� IPv4 ��ַ�ַ���, struct in_addr *inp = �洢ת����� 32 λ�����Ƶ�ַ);
 	{
		printf("invalid server_ip\n");
		return -1;
	}
	memset(tSocketServerAddr.sin_zero, 0, 8);


	iRet = connect(iSocketClient, (const struct sockaddr *)&tSocketServerAddr, sizeof(struct sockaddr));	
	if (iRet == -1) {
    perror("connect failed");  // ��ӡ��������� "connect failed: Connection refused"��
    close(iSocketClient);      // �ر��׽��֣���ֹ��Դй©��
    return -1;
    }

    
    printf("Connected to server!\n");
    fflush(stdout);//ǿ��ˢ�»�����
    
    

while (1) {
    fd_set readfds;                               
    FD_ZERO(&readfds);                            
    FD_SET(iSocketClient, &readfds);              
    FD_SET(STDIN_FILENO, &readfds);              



    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    



    int ret = select(iSocketClient + 1, &readfds, NULL, NULL, &tv);
    if (ret == -1) {
        perror("select error");
        break;
    }
    
    if (FD_ISSET(iSocketClient, &readfds)) {      
        char recvBuf[1024];
        int recvLen = recv(iSocketClient, recvBuf, sizeof(recvBuf) - 1, 0);
        if (recvLen == 0) {
            printf("Server closed the connection!\n");
            break;
        } else if (recvLen < 0) {
            perror("recv error");
            break;
        }
        recvBuf[recvLen] = '\0';
        printf("Received: %s\n", recvBuf);
        fflush(stdout); // ǿ��ˢ�»�����
    }


        if (FD_ISSET(STDIN_FILENO, &readfds))
    {
        if (fgets(ucSendBuf, sizeof(ucSendBuf) - 1, stdin)) {   
            ucSendBuf[strcspn(ucSendBuf, "\n")] = '\0';
            iSendLen = send(iSocketClient, ucSendBuf, strlen(ucSendBuf), 0);
            if (iSendLen <= 0) {
                printf("Send failed!\n");
                break;
            }
        }
    }
}
	return 0;
}

