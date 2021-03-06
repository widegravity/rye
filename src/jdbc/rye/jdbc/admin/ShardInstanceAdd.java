/*
 * Copyright 2017 NAVER Corp.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice, this list of conditions and the
 * following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice, this list of conditions and
 * the following disclaimer in the documentation and/or other materials provided with the distribution.
 *  - Neither the name of Search Solution Coporation nor the names of its contributors may be used to
 * endorse or promote products derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

package rye.jdbc.admin;

class ShardInstanceAdd extends ShardNodeAdd
{
    String commandName()
    {
	return "instanceAdd";
    }

    boolean run() throws Exception
    {
	NodeInfo[] existingNodeInfoArr = preProcessing(false);

	initDB(addNodeArr, globalDbnameArr, brokerAcl, ryeConfContents, null, existingNodeInfoArr, haGroupId, null);

	for (int i = 0; i < globalDbnameArr.length; i++) {
	    printStatus(true, "%s: add instance\n", globalDbnameArr[i]);

	    ShardMgmtInfo shardMgmtInfo = ShardMgmtInfo.find(shardMgmtInfoArr, globalDbnameArr[i]);
	    shardMgmtNodeAdd(shardMgmtInfo, globalDbnameArr[i], addNodeArr, dbaPasswordArr[i]);
	}

	brokerRestartForAddDrop(addNodeArr, globalDbnameArr[0], dbaPasswordArr[0]);

	waitDriverSynctime(System.currentTimeMillis());

	return true;
    }
}
