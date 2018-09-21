parallel ('xenial': {
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
}, 'xenial-arm': {
		node ('xenial-arm') {
		stage ('Checkout: aarch64') {
			checkout scm
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
		stage ('Compile: x86-64 (virt only)') {
			sh "SRCDIR=$WORKSPACE tools/build_x86_64_virt.sh"
		}
		stage ('NATS: x86-64 (virt only)') {
			sh "SRCDIR=$WORKSPACE tools/CI/run_nats.sh -run '.*/.*/virt/.*' -args -nemu-binary-path=$HOME/build-x86_64/x86_64_virt-softmmu/qemu-system-x86_64_virt"
		}
	}
})

