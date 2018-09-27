stage ("Builds") {
	parallel ('xenial': {
		if (!env.BRANCH_NAME.contains("experiment/automatic-removal")) {
			node ('xenial') {
				stage ('Checkout: x86-64') {
					checkout scm
				}
				stage ('Prepare: x86-64') {
					sh "sudo apt-get update"
					sh "sudo apt-get build-dep -y qemu"
				}
				stage ('Compile: x86-64') {
					sh "SRCDIR=$WORKSPACE tools/build_x86_64.sh"
				}
				stage ('NATS: x86-64') {
					sh "SRCDIR=$WORKSPACE tools/CI/run_nats.sh"
				}
			}
		}
	}, 'xenial-arm': {
			node ('xenial-arm') {
			stage ('Checkout: aarch64') {
				checkout scm
			}
			stage ('Used files: aarch64') {
				if (env.BRANCH_NAME.contains("topic/virt-x86")) {
					sh "SRCDIR=$WORKSPACE $WORKSPACE/tools/used_files.sh $WORKSPACE/tools/build_aarch64.sh > $WORKSPACE/used-aarch64.txt"
					azureUpload storageCredentialId: 'nemu-jenkins-storage-account', 
						filesPath: 'used-aarch64.txt',
						storageType: 'blobstorage',
						containerName: env.BUILD_TAG.replaceAll("%2F","-")
				}
			}
			stage ('Compile: aarch64') {
				sh "SRCDIR=$WORKSPACE tools/build_aarch64.sh"
			}
			stage ('NATS: aarch64') {
				sh "SRCDIR=$WORKSPACE tools/CI/run_nats_aarch64.sh"
			}	
		}
	}, 'xenial-virt': {
		node ('xenial') {
			stage ('Checkout: x86-64 (virt only)') {
				checkout scm
			}
			stage ('Prepare: x86-64 (virt only)') {
				sh "sudo apt-get update"
				sh "sudo apt-get build-dep -y qemu"
			}
			stage ('Used files: x86-64 (virt only)') {
				if (env.BRANCH_NAME.contains("topic/virt-x86")) {
					sh "SRCDIR=$WORKSPACE $WORKSPACE/tools/used_files.sh $WORKSPACE/tools/build_x86_64_virt.sh > $WORKSPACE/used-x86-64.txt"
					azureUpload storageCredentialId: 'nemu-jenkins-storage-account', 
						filesPath: 'used-x86-64.txt',
						storageType: 'blobstorage',
						containerName: env.BUILD_TAG.replaceAll("%2F","-")
				}
			}
			stage ('Compile: x86-64 (virt only)') {
				sh "SRCDIR=$WORKSPACE tools/build_x86_64_virt.sh"
			}
			stage ('NATS: x86-64 (virt only)') {
				sh "SRCDIR=$WORKSPACE tools/CI/run_nats.sh -run '.*/.*/virt/.*' -args -nemu-binary-path=$HOME/build-x86_64/x86_64_virt-softmmu/qemu-system-x86_64_virt"
			}
		}
	})
}

stage ("Analyse") {
	if (env.BRANCH_NAME.contains("topic/virt-x86")) { 
		node ('master'){
			stage ('Checkout: analyse') {
					checkout scm
			}
			stage ('Download results') {
				azureDownload storageCredentialId: 'nemu-jenkins-storage-account', 
						downloadType: 'container',
						containerName: env.BUILD_TAG.replaceAll("%2F","-")
			}
			stage ('Remove unused files') {
				sh "cat $WORKSPACE/used-aarch64.txt $WORKSPACE/used-x86-64.txt | sort | uniq > $WORKSPACE/used-c-files.txt"
				sh "find . | grep \\\\.[ch]\$ | sed 's#^./##' | sort | uniq > $WORKSPACE/all-c-files.txt"
				sh "comm -23 $WORKSPACE/all-c-files.txt $WORKSPACE/used-c-files.txt | tee $WORKSPACE/unused-files.txt"
				sh "git branch -D tmp-branch || true"
				sh "git checkout -b tmp-branch"
				sh 'cat $WORKSPACE/unused-files.txt | grep -v ^linux-headers | xargs -I {} -n 1 sh -c "git rm {} || true"'
				sh 'git commit -m "TEMPORARY MESSAGE" -a'
				writeFile file:'commit-message', text:"Automatic code removal\n\n"
				sh "tools/cloc-change.sh `git rev-parse remotes/origin/${env.BRANCH_NAME}` >> $WORKSPACE/commit-message"
				sh 'git commit --amend --file $WORKSPACE/commit-message'
				writeFile file:'git-helper.sh', text:"#!/bin/bash\necho username=\$GIT_USERNAME\necho password=\$GIT_PASSWORD"
				sh "chmod +x $WORKSPACE/git-helper.sh"
				sh 'git config credential.helper "/bin/bash ' + env.WORKSPACE + '/git-helper.sh"'

				withCredentials([[
					$class: 'UsernamePasswordMultiBinding',
					credentialsId: 'a27947d5-1706-465f-99f7-231eff68787b',
					usernameVariable: 'GIT_USERNAME',
					passwordVariable: 'GIT_PASSWORD'
				]]) {
					String branch_suffix = env.BRANCH_NAME.replaceAll("topic/virt-x86","")
					sh "git push origin -f tmp-branch:experiment/automatic-removal${branch_suffix}"
				}
			}
		}
	}
}

