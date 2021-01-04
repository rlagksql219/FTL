// 주의사항
// 1. sectormap.h에 정의되어 있는 상수 변수를 우선적으로 사용해야 함
// 2. sectormap.h에 정의되어 있지 않을 경우 본인이 이 파일에서 만들어서 사용하면 됨
// 3. 필요한 data structure가 필요하면 이 파일에서 정의해서 쓰기 바람(sectormap.h에 추가하면 안됨)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include "sectormap.h"

int dd_read(int ppn, char *pagebuf);
int dd_write(int ppn, char *pagebuf);
int dd_erase(int pbn);

void ftl_open();
void ftl_read(int lsn, char *sectorbuf);

void ftl_write(int lsn, char *sectorbuf);
void write_action(int lsn, char *sectorbuf, int ppn, int make_garbage);
int garbage_collection(int lsn);

void ftl_print();

typedef struct
{
	int lpn;
	int ppn;
} entry; //mapping_table을 위한 구조체

entry address_mapping_table[DATAPAGES_PER_DEVICE];

int free_block_pbn;

int free_page_stack[DATAPAGES_PER_DEVICE]; //free page 관리하는 배열
int free_page_top = -1; //스택이 비어있는 상태

int garbage_page_stack[DATAPAGES_PER_DEVICE]; //garbage page 관리하는 배열
int garbage_page_top = -1; //스택이 비어있는 상태

// 필요한 경우 헤더 파일을 추가하시오.

//
// flash memory를 처음 사용할 때 필요한 초기화 작업, 예를 들면 address mapping table에 대한
// 초기화 등의 작업을 수행한다. 따라서, 첫 번째 ftl_write() 또는 ftl_read()가 호출되기 전에
// file system에 의해 반드시 먼저 호출이 되어야 한다.
//
void ftl_open()
{
	//
	// address mapping table 초기화
	// free block's pbn 초기화
    	// address mapping table에서 lbn 수는 DATABLKS_PER_DEVICE 동일
	
	for(int i=0; i<DATAPAGES_PER_DEVICE; i++) {
		address_mapping_table[i].lpn = i;
		address_mapping_table[i].ppn = -1;

		free_page_stack[++free_page_top] = address_mapping_table[i].lpn; //처음에는 전부 free page => 전부 stack에 push
	}

	free_block_pbn = BLOCKS_PER_DEVICE - 1; //젤 마지막 block

	return;
}

//
// 이 함수를 호출하기 전에 이미 sectorbuf가 가리키는 곳에 512B의 메모리가 할당되어 있어야 한다.
// 즉, 이 함수에서 메모리를 할당받으면 안된다.
//
void ftl_read(int lsn, char *sectorbuf)
{
	char pagebuf[PAGE_SIZE];
	int ppn;

	ppn = address_mapping_table[lsn].ppn;

	if(ppn == -1) { //free page
		printf("데이터가 없습니다.\n");
		return;
	}

	else {
		dd_read(ppn, pagebuf);
	
		memcpy(sectorbuf, pagebuf, SECTOR_SIZE);
		printf("%s\n", sectorbuf);

	}

	return;
}


void ftl_write(int lsn, char *sectorbuf)
{
	int ppn;
	int make_garbage = 0;

	ppn = address_mapping_table[lsn].ppn;

	if(ppn == -1) { //최초 쓰기
		if(free_page_top >= 0) //stack is not empty
			write_action(lsn, sectorbuf, ppn, make_garbage);

		else { //stack is empty => no free page
			make_garbage = garbage_collection(lsn);
			write_action(lsn, sectorbuf, ppn, make_garbage);
		}
	}

	else { //덮어 쓰기
		if(free_page_top >= 0) //stack is not empty
			write_action(lsn, sectorbuf, ppn, make_garbage);

		else { //stack is empty => no free page
			make_garbage = garbage_collection(lsn);
			write_action(lsn, sectorbuf, ppn, make_garbage);
		}
	}

	return;
}


void write_action(int lsn, char *sectorbuf, int ppn, int make_garbage)
{
	int write_ppn;
	int garbage_ppn;
	char pagebuf[PAGE_SIZE];
	char garbage_pagebuf[PAGE_SIZE];
	SpareData write_spare;
	SpareData garbage_spare;

	write_ppn = free_page_stack[free_page_top--]; //stack에서 ppn 하나 pop

	memset(pagebuf, (char)0xFF, PAGE_SIZE); //write는 page 단위
	memcpy(pagebuf, sectorbuf, SECTOR_SIZE); //pagebuf에 sector data 저장
	write_spare.lpn = lsn;
	write_spare.is_invalid = 1;
	memcpy(pagebuf+SECTOR_SIZE, &write_spare, sizeof(SpareData)); //pagebuf에 spare data 저장

	dd_write(write_ppn, pagebuf);

	if(ppn != -1) { //덮어 쓰기
		garbage_ppn = address_mapping_table[lsn].ppn;

		memset(pagebuf, (char)0xFF, PAGE_SIZE);
		memset(garbage_pagebuf, (char)0xFF, PAGE_SIZE);
		dd_read(garbage_ppn, garbage_pagebuf);
		memcpy(pagebuf, garbage_pagebuf, PAGE_SIZE);
		garbage_spare.lpn = lsn;
		garbage_spare.is_invalid = 0; //garbage ppn marking
		memcpy(pagebuf+SECTOR_SIZE, &garbage_spare, sizeof(SpareData));

		dd_write(garbage_ppn, pagebuf);

		if(make_garbage == 0) //garbage_collection에서 garbage를 생성하지 않은 경우
			garbage_page_stack[++garbage_page_top] = garbage_ppn; //garbage page stack에 push
	}

	address_mapping_table[lsn].ppn = write_ppn; //mapping table 갱신

	return;
}


int garbage_collection(int lsn)
{
	int garbage_ppn;
	int garbage_pbn;
	char pagebuf[PAGE_SIZE];
	int make_garbage = 0;

	if(garbage_page_top < 0)  {//stack is empty => pick garbage page
		garbage_page_stack[++garbage_page_top] = address_mapping_table[lsn].ppn;
		make_garbage = 1;
	}
	
	garbage_ppn = garbage_page_stack[garbage_page_top--];
	garbage_pbn = garbage_ppn/4;

	/* free block에 유효 data copy */
	for(int i=0; i<PAGES_PER_BLOCK; i++) {
		if(4*garbage_pbn + i == garbage_ppn) {
			free_page_stack[++free_page_top] = 4*free_block_pbn + i; //새로 생성한 free page
			continue;
		}

		memset(pagebuf, (char)0xFF, PAGE_SIZE);
		
		dd_read(4*garbage_pbn + i, pagebuf);
		dd_write(4*free_block_pbn + i, pagebuf);

		/* mapping table update */
		for(int j=0; j<DATAPAGES_PER_DEVICE; j++) {
			if(4*garbage_pbn + i == address_mapping_table[j].ppn) 
				address_mapping_table[j].ppn = 4*free_block_pbn + i;
		}
	}

	/* garbage block erase */
	dd_erase(garbage_pbn);

	free_block_pbn = garbage_pbn; //free block 변경

	return make_garbage;
}


void ftl_print()
{
	printf("lpn	ppn\n");
	for(int i=0; i<DATAPAGES_PER_DEVICE; i++)
		printf("%d	%d\n", address_mapping_table[i].lpn, address_mapping_table[i].ppn);
	
	printf("free block's pbn=%d\n", free_block_pbn);

	return;
}