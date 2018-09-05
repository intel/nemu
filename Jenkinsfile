node ('xenial') {
	stage ('Checkout') {
		checkout scm
	}
	stage ('Prepare') {
		sh "sudo apt-get update"
		sh "sudo apt-get build-dep -y qemu"
	}
	stage ('Compile') {
		sh "SRCDIR=$WORKSPACE tools/build_x86_64.sh"
	}
	stage ('NATS') {
		sh "SRCDIR=$WORKSPACE tools/CI/run_nats.sh"
	}
}
