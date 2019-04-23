stage ("Builds") {
	parallel ('xenial-arm': {
			node ('xenial-arm') {
			stage ('Checkout: aarch64') {
				checkout scm
			}
			stage ('Compile: aarch64') {
				sh "SRCDIR=$WORKSPACE tools/build_aarch64.sh"
			}
			stage ('Upload binary: aarch64') {
				if (env.BRANCH_NAME.contains("pre-releases/candidate")) {
					sh "cp $HOME/build-aarch64/aarch64-softmmu/qemu-system-aarch64 ."
					azureUpload storageCredentialId: 'nemu-jenkins-storage-account', 
						filesPath: "qemu-system-aarch64",
						storageType: 'blobstorage',
						containerName: env.BUILD_TAG.replaceAll("%2F","-")
				}
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
			stage ('Compile: x86-64 (virt only)') {
				sh "SRCDIR=$WORKSPACE tools/build_x86_64_virt.sh"
			}
			stage ('Upload binary: x86-64 (virt only)') {
				if (env.BRANCH_NAME.contains("pre-releases/candidate")) {
					sh "cp $HOME/build-x86_64_virt/x86_64_virt-softmmu/qemu-system-x86_64_virt ."
					azureUpload storageCredentialId: 'nemu-jenkins-storage-account', 
						filesPath: "qemu-system-x86_64_virt",
						storageType: 'blobstorage',
						containerName: env.BUILD_TAG.replaceAll("%2F","-")
				}
			}
		}
	})
}

stage ("Release") {
	if (env.BRANCH_NAME.contains("pre-releases/candidate")) { 
		node ('master'){
			stage ('Checkout: analyse') {
					checkout scm
			}
			stage ('Download results') {
				azureDownload storageCredentialId: 'nemu-jenkins-storage-account', 
						downloadType: 'container',
						containerName: env.BUILD_TAG.replaceAll("%2F","-")
			}
			stage ('Create release') {
				writeFile file:'git-helper.sh', text:"#!/bin/bash\necho username=\$GIT_USERNAME\necho password=\$GIT_PASSWORD"
				sh "chmod +x $WORKSPACE/git-helper.sh"
				sh 'git config credential.helper "/bin/bash ' + env.WORKSPACE + '/git-helper.sh"'

				withCredentials([[
					$class: 'UsernamePasswordMultiBinding',
					credentialsId: 'a27947d5-1706-465f-99f7-231eff68787b',
					usernameVariable: 'GIT_USERNAME',
					passwordVariable: 'GIT_PASSWORD'
				]]) {
					sh "git tag pre-release-`date +%Y-%m-100`"
					sh "git push --tags origin"
				}
				withCredentials([[
					$class: 'UsernamePasswordMultiBinding',
					credentialsId: 'a27947d5-1706-465f-99f7-231eff68787b',
					usernameVariable: 'GITHUB_USERNAME',
					passwordVariable: 'GITHUB_PASSWORD'
				]]) {
					sh "hub release create -d -p -m \"Pre-release \$(date +%Y-%m-100)\" -a qemu-system-x86_64_virt -a qemu-system-aarch64 pre-release-`date +%Y-%m-100`" 
				}
			}
		}
	}
}
